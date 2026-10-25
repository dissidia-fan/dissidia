# Attacker movement: how a move moves the fighter who does it

Research note, 2026-10-24. Source: the static recompilation (`work/output/generated`, `work/overlays/battle/generated`,
read with `work/magi_re/fn.py`), the EBOOT / battle-overlay images (`work/eboot_image.bin` at 0x08804000,
`work/OVL_BATTLE_APP.bin` at 0x089DD4C0), the `spec` section of `battle.bin` (`work/mods/by_crc/93307c1c.bin`, ARC at
0x1A0, section at file 0x240), Bartz's retail `p_fiv100.objx` / `p_fiv100_1p.gmo` from `work/disc0`, and Noctis's
mod folder (`mods/noctis/moveset/p_noc100`, `mods/noctis/models/p_noc100_1p.gmo`). Nothing in the game, mods, tools or
studio was changed. Companion notes: [event-ops](../event-ops/) (per-op traces), [hit-reaction](../hit-reaction/)
(the defender's side).

Labels: **confirmed** = read end to end in code; **strong** = code read, one link inferred; **guess** = from data or
naming. Units: metres, 1 tick = 1/60 s, `dt` = `FUN_08838300` (actor clock × global speed; 1 at normal speed).
Yaw convention: forward = (sin yaw, 0, cos yaw), yaw at actor+0x9C, turned with atan2(dx, dz) (`FUN_08835CF8`).

## Summary

1. **One sum per tick** (`FUN_0883AF80`, confirmed): displacement `D = extra + stateVelocity + rootMotion + hitSlide +
   pull`, then `pos += D` through the collision sweep `FUN_0883B470`, and `velocity(+0xF0) = D / dt`.
   - `extra` (actor+0xE0 dir, +0xEC length) is zeroed at the start of every tick (`FUN_08833658`) and filled by the
     ranged ops `lunge` 0x0548 and `ground_slide` 0x044F while their range runs.
   - `stateVelocity` comes from the movement state's slot +0x50: homing state 0xF (`FUN_08A60838`), chase state 0xA
     (`FUN_08A57C70`), free fall state 1 (`FUN_08A5B59C`), and the plain ground / air-action states 0 and 0x12
     (`FUN_08804674`: stick movement only, which is zero during a move unless `steer_during_move`).
   - `rootMotion` (`FUN_08832EB8`) is the per-tick change of the **`trans_include`** bone's animated translation (and
     yaw), rotated by the actor's yaw. `trans_include` is a top-level bone with no children, so its travel never
     moves the drawn body; it only moves the actor. **Root motion is real and is where most attack travel lives**
     (Noctis's Blitz Attack: 8.3 m, Hazard Raid: 12.45 m, neither has a homing op).
2. **There is no gravity during an air move.** An air move runs in air-action state 0x12 (or homing 0xF) while
   `air_motion` 0x000A is on; those states only use root motion + `extra` + homing. The fighter hangs where the
   animation puts him. When `air_motion` ends (or was never set) the state turns into a free fall (state 1) that uses
   the **fighter's own** `spec` record: rise gravity `g = 2h/T²`, fall acceleration `g·P60·gravity_op`, terminal
   `P96` m/tick. Bartz / Noctis (record 9): h 2.5 m, T 34, P60 2.0 → **0.00865 m/tick²**, terminal 0.6 m/tick.
3. **Homing dashes** (`homing_dash` 0x0D10, `ground_homing_dash` 0x0925, the plya action homing block) all set up the
   same state 0xF (`FUN_08A60BC8`): every tick while `track > 0` the destination is re-taken from the foe's current
   position (approach direction fixed at the start), and the fighter moves at a fixed speed toward it, snapping onto it
   when it is closer than 2 × speed. It moves on `track − 1` ticks, ends after `max` ticks, and leaves with **zero
   velocity** (no carry). A {0.5, 6, …, 1.5, …, 6} dash covers at most 2.5 m (+1 m snap) and stops 1.5 m from the foe.
4. **Turning** is done by the facing ops in the event pass (`face_target` 0x0402 = max arg0°/tick toward the lock-on
   target), before the tick's movement, so `lunge`/`ground_slide` and the root motion use the new facing.
5. **Landing** during a move happens only while `landing_allowed` 0x002D is on (or in a free fall), when the height
   above the floor reaches 0 (`FUN_08833898`). It zeroes the velocity (unless `floating_physics`) and plays the
   landing motion, which cuts the move.
6. **Per-tick order** (confirmed): fighter controller → event pass for every actor (per-tick reset, keyed events,
   flag clear, ranged events) → per actor: state per-tick update → animation pose + root delta → movement sum and
   collision → ATTACK task (hit volumes against hurt volumes) after all actors have moved.

## 1. Per-tick order

`OBJ_MANAGER::ExecTask` = `FUN_088413D0` (registered by `FUN_08841CC0`, task name string 0x089C86A8) and
`ATTACK::ExecTask` = `FUN_08807538` (registered by `FUN_08808424`, priority 31000; the object tasks are 30000; the
scheduler `FUN_08882790` keeps tasks sorted by ascending priority, so ATTACK runs after all objects moved).

| Step | What runs | Address | Conf. |
|---|---|---|---|
| 1 | for every fighter: controller vtable +0x10 (`FUN_08B372D8`): input / CPU, may start an action (`FUN_08B382B8`: plays the action's script, arms the plya homing block, switches to state 0x12 / 0xF if airborne) | `FUN_088413D0` loop 1 | confirmed |
| 2 | for every actor: `FUN_08833910` → `FUN_08833658` (zero `extra` +0xE0, reset multipliers +0x3D0..+0x3DC to 1, clear per-tick bits) → motion player `FUN_08830444` (time += dt; **keyed events** whose frame is in (prev, now] (frame 0 fires on the first tick), then the flag block +0x190 is cleared, then **ranged events** with `start ≤ now < end`, `FUN_08828EEC`) | `FUN_08833910` | confirmed |
| 3 | for every actor `FUN_0883C2E8`: actor vtable +0x18 (`FUN_08B37604`: stick movement, turn toward +0x320, `FUN_08831360` → state object `FUN_08934114` → state per-tick, e.g. homing `FUN_08A60EAC`) | `FUN_0883C2E8` | strong (state per-tick call path read; exact slot through `FUN_08934114` inferred) |
| 4 | pose: `FUN_0886E5D0`, `FUN_0886AB1C`; root delta `FUN_0882F784` (only when +0x380 bit 0 is clear) | `FUN_0883C2E8` | confirmed |
| 5 | movement sum + collision + floor / landing: `FUN_0883AF80` → `FUN_0883B470` | | confirmed |
| 6 | after-move: model matrix, `FUN_08831994` updates the volume centres +0x100 (kind-1 volume, else the position) and +0x110 (kind-10 volume, else +0x100) | | confirmed |
| 7 | `ATTACK::ExecTask`: contact passes `FUN_08809228` → `FUN_08809CEC` (hit volumes of active `hit_window`s against hurt volumes) | | strong (order from the task priorities) |

So in tick *k*: the events of motion frame *k* change flags, `extra` and facing; the state update and the movement use
them in the same tick; the hit test sees the positions after this tick's movement.

## 2. The movement sum (`FUN_0883AF80`, confirmed)

```
if dt <= 0: return
prev(+0x2E0) = pos(+0x80)
D  = extra(+0xE0) * len(+0xEC)                    // lunge + ground_slide this tick
D += kind==6 ? +0xB0 : state.velocity()           // skipped when +0x380 bit 2 or +0x190 bit 0x80 (FUN_08837948)
D += rootMotion()                                 // FUN_08832EB8
D += hitSlide()                                   // FUN_08832244 (hit-reaction ground slide, see hit-reaction §3.3)
D += pull(+0x120)*(+0x12C)*dt                     // pull_foe on the victim; not for scripts 0x2BE–0x2C0, 0x2F0–0x2F2
if timer(+0x31C) > 0: D = 0                       // hit reaction 18
velocity(+0xF0) = D / dt
FUN_0883B470(actor, D)                            // sweep, floor, landing (section 6)
```

### Root motion (`FUN_0882F784` + `FUN_08832EB8`)
- `FUN_0882F8D4` reads the root-motion bone's matrix (bone index at model+0xE4, model = actor+0x394): translation =
  matrix row 3, yaw = atan2(m[2][0], m[2][2]). `FUN_0882F784` adds `current − previous` to the accumulator
  motion-player+0x10 (actor+0x140) and keeps the new value as previous (after a motion switch it differences against
  the new motion's start, +0x20). confirmed
- The bone is **`trans_include`** (bone 82 of the Bartz skeleton; Noctis's too). It has no parent and no children, so
  its animation does not move the mesh. strong (bone found by name and hierarchy, the tooling's comment "the game's
  root-motion bone"; the model+0xE4 writer was not traced)
- `FUN_08832EB8` (only while a motion plays, actor+0x178 > 0, and has not ended, +0x188 bit 0 clear): a non-zero yaw
  delta turns the actor first (`FUN_08836560`); the translation is rotated about +Y by wrap(actor yaw − root yaw)
  (yaw only), or with `root_motion_3d` 0x0062 by the actor's full orientation matrix. confirmed
- Retail motions and the Blender add-on put the travel on `trans_include` (FFXV `Trans`). Measured travel (model
  space, +z forward): Noctis `_603A` (Blitz 1) 8.28 m forward, mostly ticks 0–20; `_610A` (Hazard Raid 1) 12.45 m
  over ticks 8–44; `_652A` 4.0 m forward, back to 1.96 at 72, then +4.19 m up at 72–84; `_653A` +2.0 up, then −12 m by
  tick 44; `_602A` (FF01_NEAR3) 3.41 m straight up from tick 22. Bartz `_651A` 5.8 forward + 2.5 up, `_657A` 1.0
  forward, `_658A` 1.06 forward.

## 3. Movement states used by moves

State objects are built by `FUN_08A5D2D0` (12-byte objects {id, flags, vtable}); state vtables: 0 = 0x089C69B8,
1 = 0x08A5C298, 0xA = 0x08A580F8, 0xF = 0x08A611A8, 0x12 = 0x08A54E88. Slots: +0x08 enter, +0x18 exit, +0x30 "start
fall", +0x38 "convert to fall", +0x40 land, +0x48 per tick, +0x50 velocity. The switch is `FUN_08834C10(actor, state)`.

| State | Used for | Velocity (+0x50) | Per tick | Conf. |
|---|---|---|---|---|
| 0 ground | ground moves | `FUN_08804674`: stick vector +0xB0 × speed +0xBC (zero in a move unless 0x0518); `floating_physics`: carry +0xC0 with drag. Sets +0xC0 to the result | — | confirmed |
| 0x12 air action | air moves; entered by the action starter when airborne, `lunge`, `take_off` 0x0123, `steer_during_move`, end of homing / chase | same `FUN_08804674` (**no gravity**) | `FUN_08A54E0C`: if not "air" (`FUN_0883752C` false) → queue fall/landing scripts 101/102 and convert to fall (state +0x38 `FUN_08934054`: play 101 unless `keep_motion_when_falling` 0x0023, then free fall). Enter `FUN_08A54DDC` clears +0x384 bits 0x100/0x200 | confirmed |
| 0xF homing | 0x0D10, 0x0925, plya homing | `FUN_08A60838` (section 4) | `FUN_08A60EAC` | confirmed |
| 0xA chase | `chase_dash` 0x001D | `FUN_08A57C70`: closes the surface gap (distance − both body radii, `FUN_08837124`) linearly over 26 ticks (58 for script 813) down to about 1 m | `FUN_08A58064`: motion end → 0x12 | strong (gap target read; the 1 m end value is from the constant 1.0) |
| 1 free fall | after the air flags end, falling off a ledge | `FUN_08A5B59C` → `FUN_08A5BC58`: horizontal carry ×k per tick, vertical from the fall block (section 5) | lands via `FUN_08A5B690` | confirmed (from hit-reaction §3.3, same functions) |

"Air" (`FUN_0883752C`) = `air_motion` bit (+0x190 bit 2) **or** +0x384 bit 0x1000 (set by entering 0xF from
0x0D10 / plya homing, and by 0xA; not by 0x0925) **or** actor kind 3. "Airborne" (`FUN_088374B0`) = an airborne
state flag, a blow-away script, "air", or state 0x12.

## 4. Per-op tables

Ranged args are read every tick of `[start, end)`; keyed ops fire once at their frame.

### homing_dash 0x0D10 (keyed, 13 args) — confirmed
Handler 0x0882D8C4 → `FUN_08A60BC8(actor, groundVariant=0, fixedDir=0, alt=arg12, …)`, block J = `*(actor+0x314)+0x48`.

| arg | field | meaning | conf. |
|---|---|---|---|
| 0 | J+0x14 | horizontal speed m/tick (× dt) | confirmed |
| 1 | J+0x1C | tracking ticks: dest re-taken while ≥ 0, then −dt; velocity is 0 once ≤ 0 → moves on `track − 1` ticks when started on a whole tick | confirmed |
| 2 | J+0x20 | height of the destination above the foe's reference point | confirmed |
| 3 | J+0x24 | angle (deg → rad) of the stand-off direction about +Y | strong (always 0) |
| 4 | J+0x28 | stand-off distance | confirmed |
| 5 | J+0x2C | arrive radius: `|d| < r` sets J+0x34 bit 0x01 → state ends; −1 = off | confirmed |
| 6 | J+0x30 | max ticks: state ends when elapsed ≥ max (elapsed counts from 0 on entry, +dt per tick) | confirmed |
| 7 | J+0x18 | vertical speed; ≤ 0 → arg0 | confirmed |
| 8 | J+0x34 \|= 0x10 | no rising: velocity.y > 0 → 0; stopped (bit 0x02) if actor.y < foe.y | confirmed |
| 9 | 0x20 | no falling: velocity.y < 0 → 0; stopped if foe.y < actor.y | confirmed |
| 10 | 0x40 | skip the foe checks (no busy test, no freeze) | confirmed |
| 11 | 0x80 | stopped as soon as the foe is "busy" | confirmed |
| 12 | 0x100 | alternate target (summon / partner) | confirmed |

- **Target**: lock-on target (vtable +0x120 = actor+0x2FC), else nothing happens. **Start**: `dir` = horizontal unit
  vector foe → attacker (`−norm(FUN_08836DAC)` with y = 0), stored once. If the attacker is airborne → state 0xF at
  once; otherwise mode 6 is armed and state 0xF starts at the next keyed `enter_state` 0x0000. confirmed
- **Destination** (`FUN_08A604AC`): `foe+0x110 + rotY(angle)·dir·dist + (0, height, 0)`; foe+0x110 = the foe's kind-10
  volume centre (chest), or its kind-1 volume centre (Bartz: 0.9 m above the feet) when it has none. confirmed
- **Velocity** (`FUN_08A60838`): 0 if stopped (bit 0x02) or track ≤ 0. `d = dest − actor+0x100` (the attacker's own
  kind-1 centre). If `|d| < 2·max(hs, vs)`: return `d` (snap; 0 if `|d| ≤ 0.02`). Else `n = d/|d|`, return
  `(n.x·hs·dt, n.y·vs·dt, n.z·hs·dt)` with the no-rise / no-fall clamps. confirmed
- **Busy foe** (`FUN_08837458`): script 400–469 (dodge / guard) or +0x194 bit 0x10 (`counts_as_evading`). Busy freezes
  the destination (state data +4) unless arg10; with arg11 it stops the dash. Hit reactions are not "busy". confirmed
- **End** (`FUN_08A60EAC`): arrived, elapsed ≥ max, or the motion ended → state 0x12 (0x0D10) / 0 (0x0925). The exit
  `FUN_08A607F0` zeroes the carried velocity +0xC0: **no momentum after a homing dash**. "Stopped" (bit 0x02) does not
  end the state, it only zeroes the homing velocity (that is what `lunge`/`ground_slide` "after homing" wait for).
  confirmed

### ground_homing_dash 0x0925 (keyed, 9 args) — confirmed
Handler 0x0882D9E8: `{speed, track, angle°, dist, arrive, max, skip-checks (0x40), stop-if-busy (0x80), alt}`,
height 0, vertical speed 0 (the snap still returns the full `d`). Always enters state 0xF at once, does not set the
"air" bit, ends in state 0. Retail uses are all inside a whole-motion `ground_follow`.

### Action-start homing (plya flag +0x18 bit 0) — confirmed
`FUN_08B382B8` (0x08B385BC): same set-up from the action record +0x40 speed, +0x44 vspeed, +0x48 track, +0x4C arrive,
+0x50 max, +0x54 height, +0x58 angle (rad), +0x5C dist; flags 0x08000000 / 0x10000000 / 0x20000000 = no rise / no
fall / skip checks. Only one retail action (Vivi) uses it. Noctis's actions (flags 0x00010002) do not.

### lunge 0x0548 (ranged, 5 args) — confirmed
Handler 0x0882BDC4, `{pitch°, yaw° (unused), speed, only_without_target, after_homing}`:
- in state 0xF: with `after_homing` act only once the dash is **stopped** (bit 0x02); else with `only_without_target`
  skip if there is a target. The state is not changed.
- other states: with `only_without_target` skip if there is a target; switch to state 0x12 if not already, and set
  the air bit for this tick.
- `dir` = facing rotated up by `pitch` (+ = up, strong), `extra += dir · speed · dt`.
- So with a dummy foe as the target, every `only_without_target = 1` lunge (198 of 225 retail uses) does nothing.

### ground_slide 0x044F (ranged, 4 args) — strong
Handler 0x0882C1EC, `{angle°, speed, skip_if_target, after_homing}`: nothing while airborne; in 0xF with
`after_homing` only once stopped; else skipped when `skip_if_target` and there is a target; `extra +=
rotY(facing, angle) · speed · dt` (flat).

### face_target 0x0402 (ranged, 4 int args) — confirmed
Handler 0x08829EF8, `{max°/tick, away, even_if_evading, skip_if_foe_reeling}` on the lock-on target (+0x2FC):
skip if no target; skip if arg3 and the target is in a hit reaction (script 500–798, `FUN_08838074`); skip if arg2 = 0
and the target is busy (lifted by plya flag +0x18 bit 0x10); then turn the yaw toward the flat direction to the target
by at most `arg0·π/180·dt` (`FUN_08835F60` → `FUN_08835CF8`; arg1 → `FUN_088360BC`, which turns only while the target is in a hit reaction 500–798, see event-ops G.md 0x046D; not a turn away). Retail: `{180,0,1,0}`
snap for 1–2 ticks, then `{4..30,0,0|1,0}` tracking.

`turn_to_opponent` 0x040B (opponent +0x300), `turn_to_target` 0x046D, `turn_or_swivel` 0x0811 (arg2 = 0) are the same
turn; `face_flight_direction` 0x0210 turns yaw (and pitch) toward the velocity +0xF0 (see event-ops D/G/H).

### enter_state 0x0000 (keyed) — confirmed
Handler 0x0882CF1C: switch to the state armed in ctrl+0xF4 (0 → 1 fall, 1 → 6 dodge, 2 → 4, 5 → 0xE, 6 → 0xF homing).
For a move it matters only after a 0x0D10 that fired on the ground.

### take_off 0x0123 (keyed) — confirmed
`{only_if_no_target}`: switch to state 0x12. Sits where `ground_follow` ends and `air_motion` begins.

### air_motion 0x000A / ground_follow 0x0015 / landing_allowed 0x002D — confirmed (flags), strong (sweep details)
Dispatcher 0x08829A6C / 0x0882A9B4 / 0x0882B6E8: +0x190 bit 0x02 / +0x192 bit 0x04 / +0x194 bit 0x08, for the
range's ticks.
- `air_motion`: counts as "air": no floor snapping, no conversion to a fall, the state 0x12 is kept.
- `ground_follow` (`FUN_088379B8`, also +0x380 bit 0x100000): the sweep probes the floor below the feet so the
  fighter follows steps and slopes instead of leaving the ground. On a flat floor it changes nothing.
- `landing_allowed`: see section 6.

### stop_momentum 0x0038 / floating_physics 0x0030 / gravity 0x013B — confirmed / strong / confirmed
- 0x0038 (0x0882B7F8): zero the carried velocity +0xC0 every tick. It does not stop root motion, `extra`, homing, or
  the free fall's vertical motion (which is position-based); it kills horizontal drift. strong for the fall part.
- 0x0030 (+0x194 bit 0x20): in states 0 / 0x12 the velocity becomes the carry +0xC0 decayed by the spec drags
  (`FUN_088344E8`: no stick → × `1 − (1 − P148)·dt/2` per tick); landing keeps the velocity.
- 0x013B: gravity multiplier actor+0x3DC for the free fall (reset to 1 every tick).

### no_body_collision 0x0013 — confirmed
+0x192 bit 0x02: the push-apart resolver `FUN_0883E3BC` skips this fighter (masses 100 / 1000 with `heavy_body`).
Without it the attacker and the foe push each other apart; the resolver itself was not read (open).

### chase_dash 0x001D — confirmed (see state 0xA above)

## 5. Air physics of the attacker

- **During an air move** (state 0x12 or 0xF with the air flag): no gravity. Position = previous + root motion +
  `extra` + homing. Confirmed (`FUN_08804674` has no vertical term; 0xF's velocity is the homing vector only).
- **Free fall** (state 1), entered when the air flags go off in state 0x12 (`FUN_08A54E0C`), when a ground move's
  height above the floor reaches 0.4 m without the air flag, or when the fighter is more than 0.5 m above the floor
  on the ground state (`FUN_0883B470` 0x0883C1C0…). `FUN_08A5BE70` starts it with h = 0 (y0 = current height):

```
spec  = actor+0x39C   (FUN_08A5C24C: script 500–798 → record 0, else the fighter's own record)
g     = 2 * spec.h(+36) / spec.T(+40)^2
y(t)  = y0 − 0.5 * g * spec.P60(+60) * gravMul * t^2       // hit-reaction §3.3 with h = 0
dy    = max(y(t) − y, spec.P96(+96) * dt)                    // terminal fall
horizontal: carry(+0xC0) *= 1 − (1 − spec.P148)·dt/2          // no stick
```

- The record is chosen by the fighter's setup byte +4 (`FUN_08B39E5C` → `FUN_0884E58C`); the records' ids match the
  character numbers of the name table 0x089D6CA8 (8 Golbez h 5.5 / T 60 floaty drag 0.6, 23 Chaos body height 3.9).
  Mod characters share their base character's record (runtime/roster.cpp). strong
- 40 records of 380 bytes. Some values (h m / T ticks / P60 / P96 / drags P144, P148):

| rec | who | h | T | g (m/tick²) | P60 | fall accel | P96 | P144 / P148 |
|---|---|---|---|---|---|---|---|---|
| 0 | hit reactions | 3.75 | 42 | 0.00425 | 1.0 | 0.00425 | −0.6 | 0.1 / 0.95 |
| 1 | one100 (WoL) | 3.5 | 48 | 0.00304 | 2.5 | 0.00760 | −0.6 | 0.1 / 0.95 |
| 9 | fiv100 (Bartz, Noctis) | 2.5 | 34 | 0.00433 | 2.0 | 0.00865 | −0.6 | 0.1 / 0.95 |
| 13 | sev100 (Cloud) | 2.5 | 36 | 0.00386 | 2.2 | 0.00849 | −0.6 | 0.1 / 0.95 |
| 28 | ten110 (Yuna, Y'shtola) | 6.5 | 44 | 0.00671 | 2.2 | 0.01477 | −0.6 | 0.3 / 0.8 |
| 6 | thr200 (CoD) | 4.3 | 32 | 0.00840 | 1.1 | 0.00924 | −0.6 | 0.1 / 0.95 |

  (The fall formula uses `m`, which eases toward its target; both are 1 here.)
- Landing from a fall: `FUN_08A5B690` → landing script (102, can act from 14) or a stand.

## 6. Floor and landing during a move (`FUN_0883B470`, from 0x0883C00C) — confirmed unless marked

For fighters (kind 0/1), after the sweep:
- **Airborne state, no air flag** (e.g. a free fall): landing test `FUN_08833898` → land (`FUN_08833818`); else if the
  feet are within 0.5 m of the floor: snap onto it; else start / keep falling.
- **Air flag on** (`air_motion`, homing 0x0D10, chase): only the landing test. It passes when (+0x384 bit 0x100, set
  in free fall states, strong) **or** `landing_allowed`, the height above the floor (+0x3C0) ≤ 0 and +0x384 bit 0x200
  is clear. Otherwise the fighter is not snapped; the sweep stops him at the floor (strong: sweep `FUN_0889CF04`
  not read in detail).
- **Ground state, no air flag**: height ≥ 0.4 m above the floor and not in a hit reaction → fall (101 / 102 queued);
  else snap y to the floor and mark grounded (+0x380 bit 0x02000000).
- **Land** (`FUN_08833818`): zero the carry +0xC0 unless floating; state +0x40 (state 0x12: fighter vtable +0xC0 → the
  queued landing script; that cuts the move); snap y to the floor.

## 7. Checks against retail and Noctis data (worked by hand)

Foe = a standing dummy that is the lock-on target, same character height (reference points 0.9 m above the feet).
Distances are between the two reference points, horizontally.

### Bartz fiv100 motion 64 (`_658A`, ground, two homing steps)
Events: `ground_follow` whole; `face_target` 0–2 {180}, 2–22 {4}; keyed 0x0925 @18 {0.45, 12, 0, 1.5, −1, 12};
`ground_slide` 18–30 {0, 0.22, skip_if_target 1}; hit 20–30; 0x0925 @32 {0.25, 6, 0, 1.5}; slide 32–38 {0.25, skip 1}.
- Root motion 1.06 m forward by tick 20 (0.81 by 16). The slides are skipped (there is a target).
- Foe at 6.0 m: at tick 18 the gap to the destination is 6 − 0.97 − 1.5 ≈ 3.5 m. 0.45 m/tick for 7 ticks
  (+ the last 0.09 m of root) leaves 0.35 m < 0.9 → snap on tick 25. He stops 1.5 m from the foe at tick 25, inside
  the 20–30 hit window; the state ends at tick 30 (elapsed 12) → ground. Foe at 8 m: 11 moving ticks × 0.45 = 4.95 m,
  he ends 1.5 + 0.5 ≈ 2 m short (the max-ticks limit, not the arrive radius, ends it: arrive is −1).
- The @32 dash re-closes at most 5 × 0.25 = 1.25 m.

### Bartz fiv100 motion 63 (`_657A`, air dive)
Events: `air_motion` 0–74, `root_motion_3d` whole; keyed 0x0D10 @4 {0.67, 10, −0.6, 0, 1.14, −1, 10, vs 0 → 0.67};
`lunge` 4–12 {0, 0, 0.34, only_without_target 1, after_homing 1}; hit 4–12.
- In the air (state 0x12), so the dash starts at once on tick 4. Foe 5 m away at the same height: d = (3.86, −0.6),
  per tick ≈ (0.66, −0.10). After 4 ticks (4–7) the rest (1.21 m) is below 1.34 → snap on tick 8: he sits 1.14 m in
  front of the foe's centre and 0.6 m below it from tick 8; the hit window 4–12 covers the arrival. Track 10 allows 9
  moving ticks, enough. The lunge never runs (not stopped, and there is a target). (Run through the JS below without
  root motion: z 0.66, 1.32, 1.99, 2.65, then 3.86 at tick 8; state 0xF until tick 14.)
- Tick 14 (elapsed 10) → state 0x12: no gravity, so he hangs there, drifting only by root motion (1.0 m forward,
  along the pitched body with 0x0062). At tick 74 `air_motion` ends → free fall, record 9.
- Without a target: no dash; the lunge moves him 8 × 0.34 = 2.72 m straight ahead (ticks 4–11).

### Noctis p_noc100 motion 44 (`_602A`, FF01_NEAR3, action 1959, script 10002)
Events: `air_motion` 0–64, `keep_motion_when_falling` whole; `face_target` 0–2 {180}, 2–10 {30}; keyed 0x0D10 @2
{0.15, 18, −0.15, 0, 0.1, −1, 18, vs 0.01, skip-checks}; `lunge` 2–20 {45°, 0.07, only_without_target}; hit 24–36.
- Started on the ground; `air_motion` (on since frame 0) makes him "airborne" for the 0x0D10 test at tick 2 (the
  keyed pass sees the flags of the previous tick's ranged pass), so the dash starts at tick 2 whether the action
  starter put him in state 0 or 0x12. 17 moving ticks × 0.15 = up to
  2.55 m (+0.3 snap): with the foe within ≈ 2.9 m he ends 0.1 m from the foe's centre (the bodies then push apart),
  0.15 m lower (vertical speed 0.01 m/tick). The lunge is skipped (target).
- Root motion lifts him 3.41 m between ticks 22 and 70 (no gravity while `air_motion` is on). Tick 64: `air_motion`
  ends → free fall from about 3.4 m keeping motion 044: 0.00865 m/tick² → √(2·3.4/0.00865) ≈ 28 ticks → lands near
  tick 92, past the motion's end at 82, so the landing plays after the move. (The rising slash is carried by the
  animation's travel, not by the homing.)

### Noctis Blitz Attack (command 0xA2: actions 1934 / 1935 / 1936, motions 45 / 46 / 47)
- 1934 (`_603A`, `ground_follow` whole): keyed 0x0925 @0 {0.5, 6, 0, 1.5, −1, 6}; `ground_slide` 2–2 (an empty
  range: never runs); `face_target` 0–1 {180} then 1–15 {4, even_if_evading 0}; hit 9–20 (NEAR).
- Homing moves on ticks 0–4: ≤ 2.5 m toward 1.5 m short of the foe; **root motion adds 1.54 m by tick 4, 4.7 by 12,
  7.46 by 20, 8.28 in all**. With the foe at 4 m he reaches the 1.5 m point by about tick 3 and root motion then
  pushes him into the foe (body push, then the NEAR hit's drag carries the foe 1.3 m in front of him with `follow`).
  The action's range window 0–7.5 m fits this. Nothing ends the travel but the animation.
- 1935 (`_604A`): no homing; root 4.77 m forward over the motion; three hits. 1936 (`_605A`): root 5.13 m.
- So the whole combo travels ≈ 18 m of root motion; in the studio the dummy has to be carried by the hit drags.

### Noctis Hazard Raid (command 0xA4: actions 1939 / 3012 / 3013, motions 49 / 58 / 59)
- 1939 (`_610A`, LAND_MIDDL): `ground_follow` whole; `face_target` 0–2 {180}, 2–24 {4, even_if_evading}; hit 12–28;
  **no homing op and no plya homing**. Travel is all root motion: 12.45 m forward over ticks 8–44 (peak ≈ 1.2 m/tick
  around ticks 12–16). The action's range window 4–13 m matches the 12.45 m dash.
- 3012 (`_652A`): `ground_follow` 0–74, `air_motion` 72→; root 4.0 m forward by tick 48, back to 1.96 m at 72, then
  4.19 m up (and 2.2 m forward) at 72–84. Ground state with the air flag: no snapping, no gravity, he stays at 4.19 m.
- 3013 (`_653A`): `air_motion` whole → starts airborne in state 0x12; root up 2.0 m by tick 12 (6.2 m), then the dive:
  −12 m by tick 44. The floor is crossed at about tick 29 (4.19 + 2.0 − 6.2); `landing_allowed` starts only at 48, so
  until then the sweep holds him at the floor (strong) through the HZ_DIVE (12–26) and BR_L_RUSH3 (26–38) hits; at
  tick 48 he lands and the landing motion cuts the rest.

### What the studio cannot see from timelines alone
- The travel of most moves is in the animation's `trans_include` track; a studio that only simulates the ops will
  leave the attacker in place for Hazard Raid and Blitz Attack.

## 8. JS for the studio

Y up, metres, one call per tick, `t` = the motion's tick (the animation frame is `t/2`). `rootAt(t)` must return the
`trans_include` translation `{x,y,z}` in model space (+z forward) at frame `t/2`, and optionally `yaw`. Positions are
the feet; `refY` is the kind-1 volume height (0.9 for Bartz / Noctis). The foe object only needs `pos`, `refY`,
`busy` (dodging / guarding) and `reeling` (script 500–798). Vector helpers as in `dummy.js`.

```js
const SPEC = {                                   // battle.bin spec records (h, T, P60, P96, P148)
  9: { h: 2.5, T: 34, P60: 2.0, P96: -0.6, P148: 0.95 },      // Bartz, Noctis
  13: { h: 2.5, T: 36, P60: 2.2, P96: -0.6, P148: 0.95 },     // Cloud
  28: { h: 6.5, T: 44, P60: 2.2, P96: -0.6, P148: 0.80 },     // Yuna (Y'shtola)
};
const D2R = Math.PI / 180, wrapPi = (a) => Math.atan2(Math.sin(a), Math.cos(a));
const fwd = (yaw) => v3(Math.sin(yaw), 0, Math.cos(yaw));
const flat = (v) => v3(v.x, 0, v.z);

function makeActor(pos, yaw, specId = 9, refY = 0.9) {
  return { pos: { ...pos }, yaw, spec: SPEC[specId], refY, state: 'ground', vel: v3(0, 0, 0), carry: v3(0, 0, 0),
           extra: v3(0, 0, 0), flags: {}, J: null, armed: null, fall: null, chase: null, root: null, landed: false };
}

// Call when the move (action) starts. plyaHoming = the action's homing block when its flag +0x18 bit 0 is set.
function startMove(a, foe, firstTickRanged, plyaHoming) {
  const air = a.pos.y > 0.001 || firstTickRanged.some((e) => e.op === 'air_motion');
  a.state = air ? 'air' : 'ground'; a.root = null; a.landed = false; a.fall = null;
  if (plyaHoming && foe) startHoming(a, foe, plyaHoming, false);
}

function turnToward(a, dir, maxRad) {            // FUN_08835CF8
  if (Math.hypot(dir.x, dir.z) <= 0.01) return;
  const d = wrapPi(Math.atan2(dir.x, dir.z) - a.yaw);
  a.yaw = wrapPi(a.yaw + Math.max(-maxRad, Math.min(maxRad, d)));
}
const isAir = (a) => !!a.flags.air_motion || (a.state === 'homing' && !a.J.ground) || a.state === 'chase';
const airborne = (a) => isAir(a) || a.state === 'air' || a.state === 'fall';
const refPt = (o) => v3(o.pos.x, o.pos.y + o.refY, o.pos.z);

function startHoming(a, foe, p, ground) {        // FUN_08A60BC8 (p: event args by name, angle in degrees)
  a.J = { ground, hs: p.speed, vs: ground ? 0 : (p.vspeed > 0 ? p.vspeed : p.speed), track: p.track_ticks,
          height: ground ? 0 : (p.height || 0), angle: (p.angle || 0) * D2R, dist: p.distance,
          arrive: p.arrive_radius, max: p.max_ticks,
          noRise: !ground && !!(p.args || [])[0], noFall: !ground && !!(p.args || [])[1],   // 0x0D10 args 8, 9
          skipChecks: !!(p.args || [])[ground ? 0 : 2], stopIfBusy: !!(p.args || [])[ground ? 1 : 3],
          dir: vNorm(flat(vSub(refPt(a), refPt(foe)))), dest: null, elapsed: 0, frozen: false, arrived: false, stopped: false };
  if (ground || airborne(a)) a.state = 'homing'; else a.armed = 'homing';   // else wait for enter_state
}

// Keyed events of this tick (frame === t), in timeline order.
function startOp(a, ev, foe) {
  switch (ev.op) {
    case 'homing_dash': if (foe) startHoming(a, foe, ev, false); break;
    case 'ground_homing_dash': if (foe) startHoming(a, foe, ev, true); break;
    case 'enter_state': if (a.armed === 'homing') { a.state = 'homing'; a.armed = null; } break;
    case 'take_off': if (!(ev.only_without_target && foe)) a.state = 'air'; break;
  }
}

// One tick. ranged = the ranged events with start <= t < end; keyed = those with frame === t.
function stepActor(a, foe, t, keyed, ranged, rootAt) {
  a.extra = v3(0, 0, 0); a.gravMul = 1;                                  // FUN_08833658
  for (const ev of keyed) startOp(a, ev, foe);                           // still sees last tick's flags
  a.flags = {};                                                          // flag block cleared, then ranged
  for (const ev of ranged) {                                             // ranged handlers, in order
    const f = ev.args || [];
    switch (ev.op) {
      case 'air_motion': case 'ground_follow': case 'landing_allowed': case 'floating_physics':
      case 'keep_motion_when_falling': case 'no_body_collision': case 'root_motion_3d': a.flags[ev.op] = true; break;
      case 'stop_momentum': a.carry = v3(0, 0, 0); break;
      case 'gravity': a.gravMul = ev.multiplier ?? f[0]; break;
      case 'face_target': case 'turn_to_opponent': case 'turn_to_target': {
        if (!foe) break;
        const [deg, away, evenIfEvading, skipIfReeling] = f;
        if (ev.op === 'face_target' && skipIfReeling && foe.reeling) break;
        if (!evenIfEvading && foe.busy) break;
        const d = flat(vSub(foe.pos, a.pos));
        turnToward(a, away ? vMul(d, -1) : d, Math.min(180, deg) * D2R); break; }
      case 'face_flight_direction': if (vLen(flat(a.vel)) > 0) turnToward(a, f[1] ? vMul(a.vel, -1) : a.vel, f[0] * D2R); break;
      case 'lunge': {                                                    // 0x0548
        if (a.state === 'homing') {
          if (ev.after_homing) { if (!a.J.stopped) break; }
          else if (ev.only_without_target && foe) break;
        } else {
          if (ev.only_without_target && foe) break;
          if (a.state !== 'air') a.state = 'air';
          a.flags.air_motion = true;                                     // this tick
        }
        const p = ev.pitch * D2R, F = fwd(a.yaw);
        a.extra = vAdd(a.extra, vMul(v3(F.x * Math.cos(p), Math.sin(p), F.z * Math.cos(p)), ev.speed)); break; }
      case 'ground_slide': {                                             // 0x044F
        if (airborne(a)) break;
        if (a.state === 'homing' && ev.after_homing) { if (!a.J.stopped) break; }
        else if (ev.skip_if_target && foe) break;
        a.extra = vAdd(a.extra, vMul(fwd(a.yaw + (ev.angle || 0) * D2R), ev.speed)); break; }
      case 'chase_dash': if (a.state !== 'chase' && foe) {
        const gap = Math.max(0, vLen(flat(vSub(foe.pos, a.pos))) - 2 * 0.45);   // body radii: assumption
        a.state = 'chase'; a.chase = { t: 0, T: 26, gap0: gap, dir: vNorm(flat(vSub(foe.pos, a.pos))) }; } break;
    }
  }

  // state per-tick + velocity (FUN_08A60EAC / FUN_08A60838 / FUN_08804674 / fall)
  let sv = v3(0, 0, 0);
  if (a.state === 'homing') {
    const J = a.J;
    if (!J.skipChecks && foe) {
      if (J.noRise && a.pos.y < foe.pos.y) J.stopped = true;
      if (J.noFall && foe.pos.y < a.pos.y) J.stopped = true;
      if (J.stopIfBusy && foe.busy) J.stopped = true;
    }
    if (J.track >= 0 && !J.frozen && foe)
      J.dest = vAdd(vAdd(refPt(foe), vMul(vRotY(J.dir, J.angle), J.dist)), v3(0, J.height, 0));
    J.track -= 1;
    if (!J.skipChecks && foe && foe.busy) J.frozen = true;
    const end = J.arrived || (J.max > 0 && J.elapsed >= J.max);
    if (end) { a.state = J.ground ? 'ground' : 'air'; a.carry = v3(0, 0, 0); a.J = null; }   // exit zeroes +0xC0
    else {
      J.elapsed += 1;
      if (!J.stopped && J.track > 0 && J.dest) {
        const d = vSub(J.dest, refPt(a)), L = vLen(d);
        if (J.arrive > L) J.arrived = true;
        if (L < 2 * Math.max(J.hs, J.vs)) sv = L <= 0.02 ? v3(0, 0, 0) : d;
        else { const n = vMul(d, 1 / L); sv = v3(n.x * J.hs, n.y * J.vs, n.z * J.hs); }
        if (J.noRise && sv.y > 0) sv.y = 0;
        if (J.noFall && sv.y < 0) sv.y = 0;
      }
    }
  } else if (a.state === 'chase') {
    const C = a.chase; C.t = Math.min(C.t + 1, C.T);
    const want = 1.0 + C.gap0 * (C.T - C.t) / C.T, now = Math.max(0, vLen(flat(vSub(foe.pos, a.pos))) - 2 * 0.45);
    sv = vMul(C.dir, Math.max(0, now - want));
    if (C.t >= C.T) a.state = 'air';
  } else if (a.state === 'fall') {
    const s = a.spec, g = 2 * s.h / (s.T * s.T), F = a.fall; F.t += 1;
    const y = F.y0 - 0.5 * g * s.P60 * a.gravMul * F.t * F.t;
    a.carry = vMul(a.carry, 1 - (1 - s.P148) / 2);
    sv = v3(a.carry.x, Math.max(y - a.pos.y, s.P96), a.carry.z);
  } else if (a.flags.floating_physics) {                                // states 0 / 0x12
    a.carry = vMul(a.carry, 1 - (1 - a.spec.P148) / 2); sv = { ...a.carry };
  } else a.carry = v3(0, 0, 0);                                          // no stick in the studio
  if (a.state === 'air' && !isAir(a)) startFall(a);                      // FUN_08A54E0C (takes effect next tick)

  // root motion (FUN_0882F784 + FUN_08832EB8): trans_include delta, rotated by the yaw
  const r = rootAt(t), prev = a.root || r; a.root = r;
  if (r.yaw !== undefined && prev.yaw !== undefined) a.yaw = wrapPi(a.yaw + (r.yaw - prev.yaw));
  const rd = vRotY(vSub(r, prev), a.yaw);                                 // 0x0062 would use the full body matrix

  const D = vAdd(vAdd(a.extra, sv), rd);                                 // FUN_0883AF80
  a.pos = vAdd(a.pos, D); a.vel = D;

  // floor and landing (FUN_0883B470), floor at y = 0
  const h = a.pos.y;
  if (isAir(a) || a.state === 'air' || a.state === 'fall') {
    const canLand = a.flags.landing_allowed || a.state === 'fall';
    if (h <= 0 && canLand) { a.pos.y = 0; if (!a.flags.floating_physics) a.carry = v3(0, 0, 0);
                             a.state = 'ground'; a.fall = null; a.landed = true; }   // studio: switch to the landing motion
    else if (h < 0) a.pos.y = 0;                                          // held at the floor (sweep; strong)
  } else if (h >= 0.4) startFall(a);                                      // ground state walked / pushed off
  else a.pos.y = 0;                                                       // snap (ground_follow keeps this on slopes)
  return a;
}

function startFall(a) {                          // FUN_08934054 + FUN_08A5BE70(h = 0) -> state 1
  a.state = 'fall'; a.fall = { y0: a.pos.y, t: 0 };
  a.fallMotion = !a.flags.keep_motion_when_falling;                       // true: the game plays script 101 (fall)
}
```

Notes for the studio:
- Unwrap the JSON's float objects first (`{"f": 0.15}` → 0.15). Event names / arg names are the `timelines/*.json` names (`speed`, `track_ticks`, `height`, `angle`, `distance`,
  `arrive_radius`, `max_ticks`, `vspeed`; the flag args of 0x0D10 are `args[0..4]` = no_rise, no_fall, skip_checks,
  stop_if_busy, alt; of 0x0925 `args[0..2]` = skip_checks, stop_if_busy, alt).
- Draw the body at `a.pos`, turned by `a.yaw`, and keep the animation's `trans_include` out of the drawn pose (it has
  no children, so the skinned mesh already ignores it). The dummy's hit drags (`follow`) then see the moving attacker.
- Body push (no `no_body_collision`) is not modelled: keep the attacker's reference point at least the two body
  radii from the foe's, or let the hit drags carry the foe.

## 9. Not settled
- The exact call slot from `FUN_08934114` to the state's per-tick method (the path is read, the slot is inferred).
- The model+0xE4 (root-motion bone) writer; that it is `trans_include` comes from the bone name, the hierarchy and the
  data.
- The collision sweep `FUN_0889CF04` (floor clamp while airborne without `landing_allowed`, walls) and the push-apart
  resolver `FUN_0883E3BC`.
- Whether frame-0 ranged flags are set before the action starter's airborne test (decides state 0x12 vs 0 for moves
  that start on the ground with `air_motion` from frame 0; either way they count as airborne from tick 1).
- Chase state 0xA end gap (1.0 m read as a constant) and the body radius `FUN_08837124`.
- plya +0x24..+0x30 (distance / height windows) are not read by any movement function traced here; they look like
  the move-selection ranges (guess).
