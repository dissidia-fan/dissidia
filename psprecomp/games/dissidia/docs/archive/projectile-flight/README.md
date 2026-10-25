# Projectile (magi) flight: spawn, motion, homing, hit

Research note for the Move Studio's dummy foe: how a `magi` record (204 bytes, 46 parameter triples, see
`work/magi_re/MAGI_NOTES.md` and `tools/charmod_magi.py`) becomes bullets, how they move every tick and when and with
what they hit. Read from the recompiled battle overlay (`work/overlays/battle/generated`, `work/magi_re/fn.py <addr>`).
Nothing in the game, mods, tools or studio was changed.

Labels: **confirmed** = read in code end to end; **strong** = code read, one link inferred (VFPU matrix convention,
a field's name, a caller not traced); **guess** = from data or naming only.

Units: metre, tick = 1/60 s, `dt` = 1 at normal speed (the emitter update `FUN_08AD0BC8` passes
`[0x089D4920] × [0x089D5EAC]`, the global speed, as in `../hit-reaction/README.md`). Angles in the pool are degrees
(`× [0x08AD7620]/180`, i.e. π/180). Vectors: Y up. "Local X" of a frame is `up × F` (F = its forward).

## Summary

1. **Parameters** (`FUN_08AD0694`, confirmed): shot 0 draws every parameter as `lo + r·(hi − lo)`, `r` from the game's
   LCG (`seed = seed·0x343FD + 0x269EC3`, `r = ((seed >> 16) & 0x7FFF)/32768`, seed word 0x09AFB898). Shot k > 0 adds
   `step` to shot k−1's value when `step ≠ 0`, else redraws. A bullet keeps its shot's values for life (aim offsets
   included), except `emit_interval`, which the bullet redraws per child.
2. **Emitter** (`FUN_08AD1588`, confirmed): a timer starts at `first_delay`; each tick, while `timer ≤ 0` and fewer
   than `trunc(shots)` were made, it spawns one bullet, draws the next shot's parameters and sets `timer = interval`
   (of the new draw); then `timer −= dt`. So shot k leaves at `first_delay + Σ interval` ticks; `interval 0` = all in
   one tick.
3. **Spawn** (`FUN_08AD5D70`, confirmed structure): an origin point and forward F (14 origin cases), F replaced by
   `direction` 1/2/3, a frame `X = up × F, Y = F × X, Z = F`; `pos = origin + frame·(off_x, off_y, off_z) +
   frame·orient·(sin fwd_yaw, 0, cos fwd_yaw)·fwd_dist`; launch direction `frame·(0, −sin pitch, cos pitch)` turned
   by `launch_yaw` about Y (**positive pitch points down**), or straight at the target point for `direction 4`.
4. **Target point** (`FUN_08AD70B0`, confirmed): `T+0x110 + rotY(T.facing·aim_dist, aim_yaw) + (0, aim_height, 0)`
   for the actor chosen by `target` (0 lock target, 1 owner, 2 second target, 3 lock else second, 4 opponent;
   5 never aims). What T+0x110 is (root or body point) is **not settled** (§2.3).
5. **Per tick** (`FUN_08AD48E8`, confirmed): homing (motion 1 only) turns the direction **in 3D** toward the target
   point by at most `turn_rate`°/tick (Rodrigues about `dir × to_target`, `FUN_088690E8`) while
   `homing_start ≤ age ≤ homing_start + homing_time` (time 0 = forever) and until the distance drops under
   `homing_stop` (then homing is off for good; with flag HOMING_STOP the bullet also stops dead). HOMING_XZ keeps
   only the new x/z, HOMING_Y only the new y. Then gravity (`v = dir·speed − (0, gravity/2, 0)`, renormalised),
   then the move `pos += dir·speed` (while `age ≤ move_time` or `move_time ≤ 0`), **then** `speed += accel` (once
   `age ≥ accel_start`) and the clamp to `[speed_min, speed_max]` — `speed_max 0` stops a shot after one tick.
6. **Hit** (`FUN_08ADAD54`, `FUN_08AD12B0`, confirmed): the bullet owns one hit object (the same class melee hit
   windows use, `FUN_0880B05C`), created once when `age ≥ hit_delay` and removed when `age ≥ hit_delay + hit_time`.
   Shape: **sphere** `r = hit_radius·size` at the bullet, or, when `hit_length ≠ 0`, an **upright** volume
   (radius `hit_radius·size`, half height `hit_length·size/2`, centred `hit_length·size/2` above the bullet) — the
   old note's "capsule along the direction" is wrong. The hit object keeps the previous centre too (swept test,
   strong). atkp = `atkp`, or `atkp_reflected` after a reflect; 0xFFFF = no hitbox. On contact: `link_hit` fires
   and, without PIERCE, the bullet dies.
7. **Chains**: `link_emit` at birth (if `emit_count` = 0 and not DEATH_EMIT), `emit_count` times in flight (first at
   age `emit_delay`, then every `emit_interval`), or at death (DEATH_EMIT); `link_hit` on contact, `link_terrain*`
   on terrain, `link_fall`, `link_clash`. A child spawns from a snapshot of the parent's position, direction and size
   (`FUN_08AD021C`).
8. **Pull** (`FUN_08AD2CE0`, strong): per tick a victim inside `[pull_min_dist, pull_max_dist]` of the bullet is moved
   `pull_strength · min(1, (d − pull_min)/(pull_full − pull_min))` toward it while `pull_start ≤ age ≤ pull_start +
   pull_time` (both 0 = always).

## 1. Objects and timing

| object | class / size | fields used here | conf |
|---|---|---|---|
| emitter E | `FUN_08AD032C`, 0x130 | +0 pos / +16 dir (parent snapshot, `FUN_08AD021C`), +32 parent size, +36 record, +40 pool, +44 shots, +48 made, +52 timer, +56 origin-6 FEPA id (`origin_arg`, or the 0x0204 event's arg2 when ≠ 0, only for origin 6), +60/+64 bullet list, +68+4i drawn params | confirmed |
| bullet B | vtable 0x08ADB2A0, 0x110 (kind 4: 0x140) | +0 pos, +16 previous pos, +32 dir (unit), +48 glue offset (motion 3), +64/+68/+72 aim_dist / aim_height / aim_yaw(rad), +76 emit_count, +80 emitted, +84 last interval, +88 emit timer, +100 E, +104 owner, +108 stored target, +120 age, +128 state, +132 flags (2 homing on, 8 stopped, 0x10 dying), +140 speed, +144..+180 P15..P24 copies, +184 size, +188..+200 size params, +204 move_time, +208..+228 pull, +232 lifetime, +236 hit_time, +240 hit_delay, +256 hit object, +262 hit made | confirmed |
| hit object H | `FUN_0880B05C` (0xD0 bytes, `FUN_08807A00`) | +128 previous centre, +144 centre, +160 dir, +176 radius, +180 half height (= radius for a sphere), +184 shape (3 sphere, 1 upright), +16 bit 2 = touched | confirmed (fields), strong (meaning of +180/+184) |

A bullet's tick (`FUN_08AD2AF4` dispatches on B+128; all three end in `FUN_08ADAD54`, the hit update):

| state | code | does | conf |
|---|---|---|---|
| 0 birth | `FUN_08AD9974` → `FUN_08AD2FA8` | glue offset, floor snap (GROUND_HUG), `link_emit` at birth; **no motion** | confirmed |
| 1 flight | `FUN_08AD99A0` → `FUN_08AD30CC` | motion `FUN_08AD48E8`, size `FUN_08AD37E8`, in-flight children `FUN_08AD384C`, lingering effect, `age ≥ lifetime` → kill, stored target dead (target 5) → kill, then `age += dt` | confirmed |
| 2 death | `FUN_08AD99CC` → `FUN_08AD3234` | DEATH_EMIT child, effect hand-over | confirmed |

So the motion runs at ages 0, 1, …, lifetime: **lifetime + 1 moves** (strong: the kill goes through vtable +24, whose
effect on the next state was not read). The hit update after it sees the incremented age (birth tick = age 0). The
emitter spawns before it updates its bullet list, so a new bullet's birth tick is its spawn tick (strong).

## 2. Spawn

### 2.1 Origin (`origin`, jump table 0x08AD7668 in `FUN_08AD5D70`)

`lock` = owner vtable +288 (the lock-on target), `second` = owner vtable +304, `opponent` = owner +4360 (if owner
byte +1346 = 1) or +1492 (if 3). In the studio all three are the dummy.

| origin | point | F | conf |
|---|---|---|---|
| 0 | owner +0x80 (root position) | owner +0x90 (facing) | confirmed |
| 1 | owner bone `origin_arg` (`FUN_08836648`): matrix row 3 | row 2 (Z), normalised; up = world Y | confirmed |
| 2 | same | same; **up = the bone's row 1** (Y) — the frame tilts with the bone | confirmed |
| 3 | `clamp(A +0x80)` with A = `second` if `target` = 2, else `lock`; no lock: owner pos + facing·`tgt_dist_min` | A's facing (no lock: owner facing) | confirmed |
| 4 | same with A +0x100 | same | confirmed |
| 5 | parent snapshot E+0 | E+16 | confirmed |
| 6 | the owner's playing FEPA effect with id E+56 (`FUN_08AD3C24` → `FUN_088AA350` → matrix `FUN_088A82D8`): row 3 | row 2 | confirmed; **effect not playing → no bullet** |
| 7 | world (0, 0, 0) | (0, 0, 1) | confirmed |
| 8 / 9 | `clamp(second +0x80 / +0x100)` | second's facing | confirmed |
| 10 | summon object (`FUN_08B34DB8(owner, origin_arg)`) +0x80; none → no bullet | its +0x90 | confirmed |
| 11 / 12 | `clamp(X +0x80 / +0x100)`, X = lock, else second | X's facing | confirmed |
| 13 | opponent +0x100 (no clamp) | opponent's facing | confirmed |

`clamp(p)` (`FUN_08AD3A2C`, confirmed): `owner.pos + normalize(h)·clamp(|h|, tgt_dist_min, tgt_dist_max) + (0,
sign(dy)·clamp(|dy|, tgt_height_min, tgt_height_max), 0)` with `h` = horizontal part of `p − owner.pos`, `dy` its
height. Defaults 0..9999 leave the point as it is.

### 2.2 Direction, frame, offsets, launch

| step | rule | conf |
|---|---|---|
| `direction` | 0 keep F; 1 owner facing (+0x90); 2 owner root-matrix Z row (+0x20); 3 E+16 (parent direction); 4 keep F now, aim later | confirmed |
| frame | `Z = normalize(F)`, `X = normalize(up × Z)`, `Y = Z × X` (rows of a row-vector matrix, `v·M = v.x X + v.y Y + v.z Z`) | confirmed (code), strong (convention: fixed by `FUN_088690E8`, which must turn *toward* the target) |
| offset | `pos = point + v·frame` with `v = (off_x, off_y, off_z)`: off_z ahead, off_y up, off_x along `up × F` | confirmed |
| forward offset | `pos += (sin fwd_yaw, 0, cos fwd_yaw)·fwd_dist · orient · frame` | confirmed |
| `orient` | applies **only to the forward offset**: 1 identity (1,215 records), 0 `rotZ(90°)` (4): the fan turns vertical `(x,y,z) → (−y, x, z)`, 2 `rotX(90°)` (4): `(x,y,z) → (x, −z, y)`, −1 unused on the disc | confirmed (axes: `FUN_089A40C8` = rotZ, `FUN_089A4124` = rotY, `FUN_089A4180` = rotX), guess (product order) |
| launch, direction ≠ 4 | `d = (0, −sin pitch, cos pitch)·frame`, then turned by `launch_yaw` about world Y (`FUN_08868334`); origin 2: the yaw is applied in the bone frame instead | confirmed |
| launch, direction 4 | `d = normalize(target_point − pos)` (§2.3); no target → owner facing; pitch/yaw ignored | confirmed |
| yaw sign | `rotY(v, a) = (v.x cos a + v.z sin a, v.y, −v.x sin a + v.z cos a)` — the same as `vRotY` in `studio/dummy.js`; positive yaw turns F toward `up × F` (with facing angle f, `F = (sin f, 0, cos f)` becomes `(sin(f+a), 0, cos(f+a))`) | confirmed |
| pitch sign | positive = down; Noctis's swords (2220–2236) use −30 / −55 to rise | confirmed (code) + data |
| extras | SWEEP_ALT: up to 5 floor probes move the point; SPAWN_LOS: rays owner → point, a blocked ray fails the shot; STOP_ON_FAIL ends the emitter on a failed shot; `max_alive` (`FUN_08AD052C`) removes the oldest (LIMIT_NEWEST: newest) live bullet of that id | confirmed |

Size at birth: `size` (P25), or the parent's size with INHERIT_SIZE. Speed = `speed` (P14). Homing flag on.

### 2.3 The target point

`FUN_08AD70B0` (confirmed): `T = target actor` (table above); `point = T+0x110 + rotY(T+0x90 · aim_dist, aim_yaw) +
(0, aim_height, 0)` (`FUN_088693E4`). `target` 5 copies B+108 but then returns 0: it never aims or homes (confirmed).
The aim offsets are drawn once per shot (Cloud 213 `aim_yaw −180..180, aim_dist ±0.5, aim_height ±0.5` gives every
orb its own point).

**Open: what actor +0x110 is.** Its writer was not found (no direct stores in the generated code; probably a block copy
from a bone). Data: retail homing records use `aim_height` 0 (286 of 342) and sometimes −0.5 … −3, never large
positive values; origin 4 (+0x100) records use `off_y` 0 or slightly negative while origin 3 (+0x80, the root)
records use 0 or 1.0 — both points look like body points about 1 m up. The JS uses `MAGI_LOCK_H = 1.0` for both
(guess). If +0x110 is at the feet instead, Noctis's `aim_height 1` (2250, 2231–2236) is right on the chest; with the
guess it aims at head height. Settle it by printing actor +0x80/+0x100/+0x110 in the running recomp.

## 3. Per tick (`FUN_08AD48E8`, order as in code)

| # | step | rule | conf |
|---|---|---|---|
| 1 | homing | motion 1, homing flag on, `homing_start ≤ age`, `homing_time = 0` or `age ≤ homing_start + homing_time`, and a target point exists. `dist = |point − pos|`; `dist < homing_stop` → homing off for the rest of the life (+ stop, flag 8, with HOMING_STOP); else `t = turn(dir, point − pos, turn_rate·π/180·dt)`. HOMING_XZ: `dir = normalize(t.x, dir.y, t.z)`; else HOMING_Y: `dir = normalize(dir.x, t.y, dir.z)`; else `dir = t` | confirmed |
| 1a | turn | `FUN_088690E8`: `ang = min(acos(â·b̂), max)`, axis `normalize(a × b)`, Rodrigues rotation of a. One limit for the whole 3D angle (not yaw and pitch separately). `homing_stop` −1 or 0 never stops | confirmed |
| 2 | motion 3 | dir = the bone's Z row each tick (origin 1/2) | confirmed |
| 3 | gravity | if `age ≥ gravity_start` and not (speed = 0 and gravity = 0): `v = dir·speed + (0, −gravity·dt/2, 0)`, `speed = |v|`, `dir = v/|v|` — the pool value is twice the per-tick² drop | confirmed |
| 4 | GROUND_HUG | `dir.y = 0`, renormalised | confirmed |
| 5 | stopped | flag 8 → nothing below runs | confirmed |
| 6 | move | motion 0/1: `pos += dir·speed·dt` if `move_time ≤ 0` or `age ≤ move_time`; motion 2: no move **and steps 7–9 skipped**; motion 3: `pos = origin(now) + glue offset` (`FUN_08AD3ECC`) | confirmed (motion 3 placement: strong) |
| 7 | accel | `age ≥ accel_start` → `speed += accel·dt` (after the move: tick 0 uses the initial speed) | confirmed |
| 8 | clamp | `speed < speed_min → speed_min; speed > speed_max → speed_max` (every tick, also after gravity). All 467 moving retail records set `speed_max ≥ speed` | confirmed |
| 9 | terrain | not NO_TERRAIN and kind ≠ 4: a ray prev → pos against the map (`FUN_0889BEB0`); hit → effect, `link_terrain_1/2/4` by surface kind else `link_terrain`, bullet dies. BOUNCE: up to 5 sphere sweeps per tick (`FUN_088978C4`), dir reflected, `speed ×= bounce_speed`, child fired, no death | confirmed (which kind is the floor: guess kind 1, Cloud 213's `terrain_1` = ground blast) |
| 10 | floor snap | GROUND_HUG: snap to the floor; a drop > 1 m → die + `link_fall` | confirmed |
| then | size | `age ≥ size_start → size += size_rate·dt`, clamp `[size_min, size_max]` every tick (Noctis 2201: size 1.4 becomes 1.2 on its first flight tick) | confirmed |
| then | children | §5 | confirmed |
| then | lifetime | `age ≥ lifetime` → kill; `age += dt` | confirmed |

## 4. Hit

| item | rule | conf |
|---|---|---|
| window | hit object made once, at the first tick with `age ≥ hit_delay` (birth tick counts: age 0), removed at the first tick with `age ≥ hit_delay + hit_time`: present for `hit_delay ≤ age < hit_delay + hit_time`; never re-made | confirmed |
| no hitbox | `atkp` (or `atkp_reflected` after a reflect, if ≥ 0) = 0xFFFF → no hit object at all | confirmed |
| sphere | `hit_length = 0`: centre = bullet pos, radius `hit_radius·size` (`FUN_0880B3FC`, type 3) | confirmed |
| upright | `hit_length ≠ 0`: centre `pos + (0, hit_length·size/2, 0)`, radius `hit_radius·size`, half height `hit_length·size/2` (`FUN_0880B494`, type 1). Bartz 488 (r 2, len 4) is a pillar 4 m tall standing on its point; Noctis 2198 (r 1.2, len 4, size 1.2) a 4.8 m column of r 1.44 | confirmed (numbers), guess (cylinder vs capsule: the overlap test lives in the collision system, unread) |
| sweep | each update moves the old centre to H+128 before writing the new one: the collision pass can sweep prev → current. Needed in the studio: Noctis 2250 moves 3 m/tick with r 1 | strong |
| size | the size of the same tick (size runs before the hit update) | confirmed |
| victims | the hit object is the melee class; contact runs in the same pass as melee hits (`FUN_08809CEC`: the defender's kind-3 then kind-2 volumes, see `../hit-reaction/README.md`) | strong |
| on contact (H+16 bit 2) | owner / reflect / guard checks, then `link_hit` (`FUN_08AD37A8`) and, without PIERCE, the bullet is destroyed (`FUN_08ADA06C` + kill). `FUN_08ADA0A4` (clash / reflect, unread) can also destroy it | confirmed (link and PIERCE), strong (the checks) |
| repeat hits | not read; assume one hit per defender per hit object, like a melee window | guess |

## 5. Shots and chains

| link | when | conf |
|---|---|---|
| shots / first_delay / interval | §Summary 2; each shot re-runs the whole spawn with its own parameters (owner pose of that tick) | confirmed |
| `link_emit` at birth | `FUN_08AD2FA8`, if not DEATH_EMIT and `emit_count` = 0 | confirmed |
| `link_emit` in flight | `FUN_08AD384C`: `emit_count` (trunc) children; timer starts at `emit_delay`; while `timer ≤ 0`: fire, redraw `emit_interval` (first child: random; later: + step, or redraw if step = 0), `timer = interval`; then `timer −= dt` | confirmed |
| `link_emit` at death | DEATH_EMIT (`FUN_08AD3234`) | confirmed |
| `link_hit` | contact (§4), also with PIERCE | confirmed |
| `link_terrain*`, `link_fall`, `link_clash` | §3 steps 9–10; clash: a CLASH_ATTACKER bullet cancels a CLASH_VICTIM one (`FUN_08AD0CE4`) | confirmed |
| child placement | the child is a new emitter (`FUN_08AD2B84` → `FUN_08AD0978`) whose E+0/E+16/E+32 = parent pos/dir/size at that moment (`FUN_08AD021C`); origin 5 / direction 3 read them, INHERIT_SIZE reads E+32 | confirmed |
| GIVE_EFFECT | the child takes over the dying bullet's lingering effect (B+112 → E+284) | confirmed |

### Alterna (Noctis, timeline 089: 2271 @2, 2272 @8, 2274 @142; 085: 2267 @40)

* **2271** (origin 11, `off_y 3`, motion 2, no atkp, life 160): parks 3 m above the dummy's root (clamped to 25 m
  horizontally / 10 m vertically from Noctis); fires **2268** at birth.
* **2268** (origin 5 = the same point, PIERCE, PULL_FIELD): sphere r 4.8 for ages 6 … 141; pulls the dummy
  `0.2·min(1, d/3.75)` m/tick toward it within 25 m for ages 0 … 146; `link_emit 2278` at age 80 (→ 2279 → 2280,
  visuals without atkp).
* **2272** → 9 × **2273** (`emit_delay 1`, `emit_interval 9`, random `launch_pitch ±50`, `launch_yaw ±180`): visuals.
* **2274** (origin 11, `off_y 3`): sphere r 5.25 for ages 0 … 5, the finisher; `link_emit 2275` at birth (visual).
* **2267** (085, origin 11, `off_y 3`): sphere r 3.9 for ages 24 … 29; on contact `link_hit 2277` (r 3, PULL_FIELD
  0.1·min(1, d/1.5) within 9 m, ages 6 … 36 of 2277); `link_emit 2276` at age 44 (visual).
* 2269 / 2270 (origin 4 / 0, no atkp, life 9999) are effect holders.

## 6. Pull (`FUN_08AD2CE0`, strong)

Active when PULL_FIELD and (`pull_start = pull_time = 0` and the bullet is alive, or `pull_start ≤ age ≤ pull_start +
pull_time`). PULL_COND additionally filters the victim by its action (`FUN_0883809C`). With `d = bullet.pos −
victim+0x100`: nothing outside `pull_min_dist ≤ |d| ≤ pull_max_dist`; else the result is the direction `d/|d|` and the
amount `pull_strength · k`, `k = 1` if `pull_full_dist ≤ pull_min_dist`, else `min(1, (|d| − pull_min)/(pull_full −
pull_min))` — full strength from `pull_full_dist` outward, fading to 0 at `pull_min_dist`. That the caller moves the
victim by direction × amount each tick is inferred (caller not read).

## 7. Worked checks (run with the JS below, dummy 12 m ahead, `MAGI_LOCK_H` 1)

| record | by hand | JS |
|---|---|---|
| Noctis 2250 (warp throw: origin 1 bone 49, direction 4, speed 1.5 accel 0.3 max 3, turn 30, life 6, r 1, not PIERCE) | speeds per move 1.5, 1.8, 2.1, 2.4, 2.7, 3.0, 3.0 (7 moves, ages 0–6) = **16.5 m** range; aims at the point from birth | flies 1.48, 3.26, 5.33, 7.69, 10.36, 13.32 m; contact at age 6 only through the swept test (centre passes 0.6 m above the capsule top) |
| WoL Fire one100#10 (origin 0, direction 2, off (0, 1.5, 1), speed 0.35 → 0.5 by 0.02 from age 4, turn 0.5, stop 1, size 2, r 0.5, life 180) | 5×0.35 + 0.37…0.49 (7) + 169×0.5 = **89.3 m**; hit sphere 0.5×2 = **1.0 m**; turns at most 0.5°/tick = 90° over its life | no target: 89.26 m, ends (0, 1.5, 90.26) |
| Noctis 475 → 476 (fan of 5, interval 2, `fwd_yaw 60 −30/shot`, `launch_yaw 40 −20/shot`, origin 6, speed 0, life 20, DEATH_EMIT) | spawn points 1 m around the effect at yaw 60, 30, 0, −30, −60 on ticks 0, 2, 4, 6, 8; each waits 20 ticks then fires 476 along its launch yaw (40 … −40) | effect stub at (0, 1.2, 0.3): (0.87, 1.4, 0.80), (0.50, 1.4, 1.17), (0, 1.4, 1.30)…; 476 leaves at tick 22, 24, … and starts homing at age 8 (3°/tick, stops within 3 m) |
| Bartz 486 → 487 → 488 | 487: 4 shots `fwd_dist 9`, `fwd_yaw 0 −90/shot` = a ring at 9 m; 488 r 2 len 4 = 4 m pillars | — |
| Alterna 2271 / 2274 (dummy at (3, 0, 10)) | both park at (3, 3, 10); 2268 r 4.8 from age 6, 2274 r 5.25 ages 0–5 | same |

## 8. What the studio's `fxShot` does differently (fx.js)

* uses `launch_yaw` for the forward offset (the game: `fwd_yaw`), ignores `launch_pitch`, `orient`, `direction`,
  origins 3–5 and 7–13, homing, the parameter steps (every shot gets shot 0's values), and `aim_*`;
* integrates speed analytically from age 0 (the game moves before accelerating) and treats `speed_max 0` as
  unlimited (the game stops the shot);
* gravity: `y −= g·t²/2` from `gravity_start`; the game adds `g/2` per tick to the velocity (half as strong) and the
  clamp limits the falling speed;
* no hit volume (projectiles don't hit the dummy).

## 9. JS for the studio

Drop-in, no dependencies (plain `{x, y, z}` like `dummy.js`). Records are the `projectiles.json` objects (`values` =
`[lo, hi, step]` per parameter). `spawnShots(record, owner, target, opts, fireTick)` returns bullets with their spawn
tick (`shot.tick`); call `stepShot(shot, target)` once per tick from that tick on. It returns position, velocity, the
active hit volume and the child events; feed each event back into `spawnShots(byId[e.fire], owner, target,
{ parent: e.parent }, tick + 1)`. Test contact with `magiTouches(hit, foe.pos)` (segment vs the dummy's 0.25–1.45 m
r 0.3 capsule; sphere hits are swept from the previous centre), report it with `shotContact(shot)` (returns the
`link_hit` events, kills a non-PIERCE shot), and move the dummy by `magiPull(shot, foe)` each tick.

Actors: `{ pos, fwd, axisZ?, centre?, aim?, bone?(id) }`. For a three.js matrix `m` (bone or FEPA effect),
`{ X: col0, Y: col1, Z: col2, T: col3 }` (the game's row-vector rows are three.js columns); `opts.effect(id)` must
return that for origin 6 (or `null` when the effect isn't playing). `owner` and `target` may be functions of the
tick; pass `stepShot(shot, null, tick)` to re-evaluate them every tick.

```js
/* Magi projectile flight for the Move Studio (docs/archive/projectile-flight/README.md).
   Metres, ticks (1/60 s, dt = 1), the record's angles in degrees. Vectors are {x, y, z}; an actor is
   { pos, fwd, axisZ?, centre?, aim?, bone?(id) -> {X, Y, Z, T} }  (fwd = +0x90 facing, axisZ = root matrix Z row +0x20,
   centre = +0x100, aim = +0x110; bone rows = three.js matrix columns). */
const MAGI_PARAMS = ['shots', 'first_delay', 'interval', 'fwd_dist', 'fwd_yaw', 'off_x', 'off_y', 'off_z', 'launch_pitch',
  'launch_yaw', 'aim_dist', 'aim_yaw', 'aim_height', 'move_time', 'speed', 'speed_min', 'speed_max', 'accel', 'accel_start',
  'turn_rate', 'homing_start', 'homing_time', 'homing_stop', 'gravity', 'gravity_start', 'size', 'size_min', 'size_max',
  'size_rate', 'size_start', 'emit_count', 'emit_delay', 'emit_interval', 'tgt_dist_min', 'tgt_dist_max', 'tgt_height_min',
  'tgt_height_max', 'pull_start', 'pull_time', 'pull_strength', 'pull_full_dist', 'pull_min_dist', 'pull_max_dist',
  'lifetime', 'hit_time', 'hit_delay'];
const MF = { PIERCE: 0x1, NO_TERRAIN: 0x2, GROUND_HUG: 0x4, DEATH_EMIT: 0x20, BOUNCE: 0x800, HOMING_XZ: 0x2000,
  HOMING_Y: 0x4000, INHERIT_SIZE: 0x20000, STOP_ON_FAIL: 0x80000, PULL_FIELD: 0x400000, HOMING_STOP: 0x8000000 };
const MAGI_LOCK_H = 1.0;            // GUESS: height of an actor's +0x100 / +0x110 points above its root (see README §2.3)
const MAGI_FOE = { lo: 0.25, hi: 1.45, r: 0.3 };   // the dummy's hurt capsule (as dummy.js)
const MD2R = Math.PI / 180;

const mv = (x = 0, y = 0, z = 0) => ({ x, y, z });
const mAdd = (a, b) => mv(a.x + b.x, a.y + b.y, a.z + b.z), mSub = (a, b) => mv(a.x - b.x, a.y - b.y, a.z - b.z);
const mMul = (a, k) => mv(a.x * k, a.y * k, a.z * k), mDot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const mCross = (a, b) => mv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
const mLen = (a) => Math.sqrt(mDot(a, a)), mNorm = (a) => { const l = mLen(a); return l > 0 ? mMul(a, 1 / l) : mv(); };
const mRotY = (a, t) => { const c = Math.cos(t), s = Math.sin(t); return mv(a.x * c + a.z * s, a.y, -a.x * s + a.z * c); };
const mClamp = (x, lo, hi) => (x < lo ? lo : x <= hi ? x : hi);           // the game's clamp (lo wins if lo > hi)
const mFlags = (r) => (typeof r.flags === 'string' ? parseInt(r.flags, 16) : r.flags) >>> 0;
const mAt = (x, t) => (typeof x === 'function' ? x(t) : x);

/* the game's LCG (word 0x09AFB898): seed = seed*0x343FD + 0x269EC3, value = (seed >> 16 & 0x7FFF) / 32768 */
let magiSeed = 1;
function magiRand() { magiSeed = (Math.imul(magiSeed, 0x343FD) + 0x269EC3) | 0; return ((magiSeed >>> 16) & 0x7FFF) / 32768; }

/* FUN_08AD0694: the 46 parameters of shot k; a later shot adds `step` instead of redrawing */
function magiDraw(rec, k, prev) {
  const P = {};
  for (const n of MAGI_PARAMS) {
    const [lo, hi, st] = rec.values[n];
    P[n] = k === 0 || st === 0 ? lo + magiRand() * (hi - lo) : prev[n] + st;
  }
  return P;
}
const aPt = (A, which) => which === 256 ? (A.centre || mAdd(A.pos, mv(0, MAGI_LOCK_H, 0)))
  : which === 272 ? (A.aim || mAdd(A.pos, mv(0, MAGI_LOCK_H, 0))) : A.pos;

/* FUN_08AD70B0: the target point = target +0x110 + rotY(target facing * aim_dist, aim_yaw) + (0, aim_height, 0) */
function magiTargetActor(rec, ctx) {
  switch (rec.target) {
    case 0: return ctx.lock; case 1: return ctx.owner; case 2: return ctx.second;
    case 3: return ctx.lock || ctx.second; case 4: return ctx.opponent;
    default: return null;                  // 5: the code returns 0 -> never aims / homes
  }
}
function magiAimPoint(rec, P, ctx) {
  const T = magiTargetActor(rec, ctx); if (!T) return null;
  const off = mRotY(mMul(T.fwd, P.aim_dist), P.aim_yaw * MD2R);
  return mAdd(mAdd(aPt(T, 272), off), mv(0, P.aim_height, 0));
}

/* FUN_08AD5D70 origin cases (jump table 0x08AD7668): {pos, F, up} or null when the shot can't spawn */
function magiOrigin(rec, P, ctx) {
  const o = ctx.owner, up = mv(0, 1, 0);
  const clampTo = (pt) => {                                               // FUN_08AD3A2C
    const h = mv(pt.x - o.pos.x, 0, pt.z - o.pos.z), dy = pt.y - o.pos.y;
    const hd = mClamp(mLen(h), P.tgt_dist_min, P.tgt_dist_max), vd = mClamp(Math.abs(dy), P.tgt_height_min, P.tgt_height_max);
    return mAdd(mAdd(o.pos, mMul(mNorm(h), hd)), mv(0, Math.sign(dy) * vd, 0));
  };
  const onActor = (A, which) => A ? { pos: clampTo(aPt(A, which)), F: A.fwd, up }
    : { pos: mAdd(o.pos, mMul(o.fwd, P.tgt_dist_min)), F: o.fwd, up };
  switch (rec.origin) {
    case 0: return { pos: o.pos, F: o.fwd, up };
    case 1: case 2: {
      const B = o.bone && o.bone(rec.origin_arg);
      if (!B) return { pos: o.pos, F: o.fwd, up };
      return { pos: B.T, F: mNorm(B.Z), up: rec.origin === 2 ? mNorm(B.Y) : up };
    }
    case 3: case 4: return onActor(rec.target === 2 ? ctx.second : ctx.lock, rec.origin === 3 ? 128 : 256);
    case 5: return ctx.parent ? { pos: ctx.parent.pos, F: ctx.parent.dir, up } : { pos: o.pos, F: o.fwd, up };
    case 6: {                                                              // FUN_08AD3C24: the playing FEPA effect's matrix
      const M = ctx.effect && ctx.effect(ctx.arg2 || rec.origin_arg);
      return M ? { pos: M.T, F: M.Z, up } : null;                         // effect not playing -> no shot
    }
    case 7: return { pos: mv(), F: mv(0, 0, 1), up };
    case 8: case 9: return onActor(ctx.second, rec.origin === 8 ? 128 : 256);
    case 10: return ctx.summon ? { pos: ctx.summon.pos, F: ctx.summon.fwd, up } : null;
    case 11: case 12: return onActor(ctx.lock || ctx.second, rec.origin === 11 ? 128 : 256);
    case 13: return ctx.opponent ? { pos: aPt(ctx.opponent, 256), F: ctx.opponent.fwd, up } : { pos: o.pos, F: o.fwd, up };
  }
  return null;
}

/* FUN_08AD5D70 tail + FUN_08AD46F0: one bullet */
function magiBirth(rec, P, ctx, tick) {
  const fr = magiOrigin(rec, P, ctx); if (!fr) return null;
  const o = ctx.owner;
  let F = fr.F;
  if (rec.direction === 1) F = o.fwd;
  else if (rec.direction === 2) F = o.axisZ || o.fwd;
  else if (rec.direction === 3) F = ctx.parent ? ctx.parent.dir : o.fwd;           // top level: emitter +16 (GUESS owner facing)
  const Z = mNorm(F), X = mNorm(mCross(fr.up, Z)), Y = mCross(Z, X);
  const L = (q) => mAdd(mAdd(mMul(X, q.x), mMul(Y, q.y)), mMul(Z, q.z));
  const fy = P.fwd_yaw * MD2R;
  let fw = mv(Math.sin(fy) * P.fwd_dist, 0, Math.cos(fy) * P.fwd_dist);
  if (rec.orient === 0) fw = mv(-fw.y, fw.x, fw.z);                         // rotZ(90) (GUESS order)
  else if (rec.orient === 2) fw = mv(fw.x, -fw.z, fw.y);                    // rotX(90)
  const pos = mAdd(mAdd(fr.pos, L(mv(P.off_x, P.off_y, P.off_z))), L(fw));
  let dir;
  if (rec.direction === 4) {
    const A = magiAimPoint(rec, P, ctx);
    dir = A ? mNorm(mSub(A, pos)) : o.fwd;
  } else {
    const p = P.launch_pitch * MD2R, y = P.launch_yaw * MD2R, lp = mv(0, -Math.sin(p), Math.cos(p));   // +pitch = down
    dir = rec.origin === 2 ? L(mRotY(lp, y)) : mRotY(L(lp), y);
  }
  const f = mFlags(rec), parent = ctx.parent;
  return { rec, P, f, ctx, tick, state: 0, age: 0, pos, prev: pos, dir: mNorm(dir), speed: P.speed,
    size: (f & MF.INHERIT_SIZE) && parent ? parent.size : P.size, homing: true, stopped: false,
    emitCount: Math.trunc(P.emit_count), emitN: 0, emitT: P.emit_delay, emitIv: 0,
    hitMade: false, hitOn: false, hitC: null, reflected: false };
}

/* FUN_08AD1588: the emitter. owner / target: actor objects or functions of the tick.
   opts: { parent: {pos, dir, size}, effect(id) -> {X,Y,Z,T}, arg2, floorY } */
function spawnShots(rec, owner, target, opts = {}, fireTick = 0) {
  const out = [];
  let P = magiDraw(rec, 0, null), k = 0, timer = P.first_delay;
  const n = Math.trunc(P.shots);
  for (let u = 0; k < n && u < 4000; u++) {
    while (timer <= 0 && k < n) {
      const T = fireTick + u, tgt = mAt(target, T);
      const ctx = { owner: mAt(owner, T), lock: tgt, second: tgt, opponent: tgt, parent: opts.parent, effect: opts.effect,
        arg2: opts.arg2, summon: opts.summon, floorY: opts.floorY === undefined ? 0 : opts.floorY, ownerAt: owner, targetAt: target, opts };
      const s = magiBirth(rec, P, ctx, T);
      if (s) out.push(s); else if (mFlags(rec) & MF.STOP_ON_FAIL) { k = n; break; }
      P = magiDraw(rec, k + 1, P); timer = P.interval; k++;
    }
    timer -= 1;
  }
  return out;
}

/* Rodrigues turn of unit a toward unit b by at most maxAng (FUN_088690E8) */
function magiTurn(a, b, maxAng) {
  const d = mDot(a, b), ang = Math.min(d >= 1 ? 0 : d <= -1 ? Math.PI : Math.acos(d), maxAng);
  const k = mNorm(mCross(a, b)), c = Math.cos(ang), s = Math.sin(ang);
  return mAdd(mAdd(mMul(a, c), mMul(mCross(k, a), s)), mMul(k, mDot(k, a) * (1 - c)));
}

/* FUN_08AD48E8 */
function magiMotion(s, ev) {
  const r = s.rec, P = s.P, f = s.f, ctx = s.ctx;
  if (r.motion === 1 && s.homing && P.homing_start <= s.age && (P.homing_time === 0 || s.age <= P.homing_start + P.homing_time)) {
    const A = magiAimPoint(r, P, ctx);
    if (A) {
      const d = mSub(A, s.pos), dl = mLen(d);
      if (dl < P.homing_stop) { s.homing = false; if (f & MF.HOMING_STOP) s.stopped = true; }
      else {
        const t = magiTurn(s.dir, mNorm(d), P.turn_rate * MD2R);
        s.dir = f & MF.HOMING_XZ ? mNorm(mv(t.x, s.dir.y, t.z)) : f & MF.HOMING_Y ? mNorm(mv(s.dir.x, t.y, s.dir.z)) : t;
      }
    }
  }
  if (r.motion === 3 && (r.origin === 1 || r.origin === 2)) {           // glued: direction = the bone's Z row
    const B = ctx.owner.bone && ctx.owner.bone(r.origin_arg); if (B) s.dir = mNorm(B.Z);
  }
  if (s.age >= P.gravity_start && (s.speed !== 0 || P.gravity !== 0)) {   // v = dir*speed - (0, g/2, 0)
    const vel = mAdd(mMul(s.dir, s.speed), mv(0, -P.gravity * 0.5, 0));
    s.speed = mLen(vel); s.dir = mNorm(vel);
  }
  if (f & MF.GROUND_HUG) s.dir = mNorm(mv(s.dir.x, 0, s.dir.z));
  if (s.stopped) return;
  if (r.motion === 2) return;                                             // parked: no move, no accel, no terrain
  if (r.motion === 3) {                                                   // GUESS: re-place at the origin every tick
    const fr = magiOrigin(r, P, ctx); if (fr) s.pos = mAdd(fr.pos, s.glue || (s.glue = mSub(s.pos, fr.pos)));
  } else if (P.move_time <= 0 || s.age <= P.move_time) s.pos = mAdd(s.pos, mMul(s.dir, s.speed));
  if (s.age >= P.accel_start) s.speed += P.accel;
  s.speed = mClamp(s.speed, P.speed_min, P.speed_max);                    // speed_max 0 stops the shot
  if (!(f & MF.NO_TERRAIN) && r.kind !== 4 && ctx.floorY !== null && s.pos.y < ctx.floorY && s.prev.y >= ctx.floorY) {
    // terrain: a ray from the previous position (FUN_0889BEB0); the studio's only terrain is the floor (GUESS: kind 1)
    const link = r.link_terrain_1 >= 0 ? r.link_terrain_1 : r.link_terrain;
    const u = (s.prev.y - ctx.floorY) / (s.prev.y - s.pos.y), at = mAdd(s.prev, mMul(mSub(s.pos, s.prev), u));
    if (link >= 0) ev.push({ fire: link, parent: { pos: at, dir: s.dir, size: s.size } });
    if (f & MF.BOUNCE) { s.dir = mv(s.dir.x, -s.dir.y, s.dir.z); s.speed *= r.bounce_speed; s.pos = mv(s.pos.x, 2 * ctx.floorY - s.pos.y, s.pos.z); }
    else { s.pos = at; s.state = 2; }
  }
  if (f & MF.GROUND_HUG && ctx.floorY !== null) s.pos = mv(s.pos.x, ctx.floorY, s.pos.z);
}

/* FUN_08AD37E8 size, FUN_08AD384C in-flight children */
function magiSizeAndEmits(s, ev) {
  const P = s.P, r = s.rec;
  if (s.age >= P.size_start) s.size += P.size_rate;
  s.size = mClamp(s.size, P.size_min, P.size_max);
  if (r.link_emit < 0) return;
  const [lo, hi, st] = r.values.emit_interval;
  while (s.emitN < s.emitCount && s.emitT <= 0) {
    ev.push({ fire: r.link_emit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
    s.emitIv = s.emitN === 0 || st === 0 ? lo + magiRand() * (hi - lo) : s.emitIv + st;
    s.emitT = s.emitIv; s.emitN++;
  }
  s.emitT -= 1;
}

/* FUN_08AD12B0 / 0880B3FC / 0880B494: the hit shape (null when the record has no atkp) */
function magiHitShape(s) {
  const r = s.rec, atkp = s.reflected && r.atkp_reflected !== 0xFFFF ? r.atkp_reflected : r.atkp;
  if (atkp === 0xFFFF || atkp < 0) return null;
  const R = r.hit_radius * s.size;
  if (!r.hit_length) return { type: 'sphere', c: s.pos, c0: s.prev, r: R, atkp };
  const h = r.hit_length * s.size * 0.5;
  return { type: 'upright', c: mAdd(s.pos, mv(0, h, 0)), c0: mAdd(s.prev, mv(0, h, 0)), r: R, halfH: h, atkp };
}

/* one tick of a bullet: birth (FUN_08AD2FA8) / flight (FUN_08AD30CC) / death (FUN_08AD3234), then the hit (FUN_08ADAD54).
   Returns { pos, vel, dir, speed, size, hit, alive, events } ; events = [{fire: id, parent}] for spawnShots(byId[id], ...) */
function stepShot(s, target, tick) {
  const ev = [], r = s.rec, P = s.P;
  if (tick !== undefined) { const T = mAt(s.ctx.targetAt, tick); s.ctx.owner = mAt(s.ctx.ownerAt, tick); s.ctx.lock = s.ctx.second = s.ctx.opponent = T; }
  else if (target) s.ctx.lock = s.ctx.second = s.ctx.opponent = target;
  s.prev = s.pos;
  if (s.state === 0) {
    if (r.link_emit >= 0 && !(s.f & MF.DEATH_EMIT) && !(P.emit_count > 0)) ev.push({ fire: r.link_emit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
    s.state = 1;
  } else if (s.state === 1) {
    magiMotion(s, ev);
    magiSizeAndEmits(s, ev);
    if (s.age >= P.lifetime) s.state = 2;
    s.age += 1;
  } else if (s.state === 2) {
    if (r.link_emit >= 0 && s.f & MF.DEATH_EMIT) ev.push({ fire: r.link_emit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
    s.state = 3;
  }
  let hit = null;
  if (s.state <= 2) {
    if (!s.hitMade && s.age >= P.hit_delay) s.hitMade = s.hitOn = true;
    if (s.hitOn && s.age >= P.hit_delay + P.hit_time) s.hitOn = false;
    if (s.hitOn) { hit = magiHitShape(s); if (hit) hit.fresh = !s.touched; }
  }
  return { pos: s.pos, vel: mMul(s.dir, s.speed), dir: s.dir, speed: s.speed, size: s.size, hit, alive: s.state <= 2, events: ev };
}

/* the caller reports a contact: link_hit child, and the bullet dies unless PIERCE */
function shotContact(s) {
  const ev = []; s.touched = true;                                        // GUESS: one hit per foe per hit object
  if (s.rec.link_hit >= 0) ev.push({ fire: s.rec.link_hit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
  if (!(s.f & MF.PIERCE)) { s.state = 3; s.hitOn = false; }
  return ev;
}

/* contact with the dummy: closest distance between the shot's segment (swept centre, or the upright's axis) and the
   foe's capsule (pos + 0.25..1.45 m, r 0.3) */
function segDist(p1, q1, p2, q2) {
  const d1 = mSub(q1, p1), d2 = mSub(q2, p2), r = mSub(p1, p2), a = mDot(d1, d1), e = mDot(d2, d2), f = mDot(d2, r);
  let s = 0, t = 0;
  if (a < 1e-12 && e < 1e-12) return mLen(r);
  if (a < 1e-12) t = mClamp(f / e, 0, 1);
  else {
    const c = mDot(d1, r);
    if (e < 1e-12) s = mClamp(-c / a, 0, 1);
    else { const b = mDot(d1, d2), den = a * e - b * b; s = den > 1e-12 ? mClamp((b * f - c * e) / den, 0, 1) : 0; t = (b * s + f) / e;
      if (t < 0) { t = 0; s = mClamp(-c / a, 0, 1); } else if (t > 1) { t = 1; s = mClamp((b - c) / a, 0, 1); } }
  }
  return mLen(mSub(mAdd(p1, mMul(d1, s)), mAdd(p2, mMul(d2, t))));
}
function magiTouches(hit, foePos) {
  const a = mAdd(foePos, mv(0, MAGI_FOE.lo, 0)), b = mAdd(foePos, mv(0, MAGI_FOE.hi, 0));
  const [p, q] = hit.type === 'sphere' ? [hit.c0, hit.c] : [mSub(hit.c, mv(0, hit.halfH, 0)), mAdd(hit.c, mv(0, hit.halfH, 0))];
  return segDist(p, q, a, b) <= hit.r + MAGI_FOE.r;
}

/* FUN_08AD2CE0: the pull on a victim this tick (displacement toward the bullet, or null) */
function magiPull(s, victim) {
  const P = s.P; if (!(s.f & MF.PULL_FIELD) || s.state > 2) return null;
  const on = P.pull_start === 0 && P.pull_time === 0 ? true : P.pull_start <= s.age && s.age <= P.pull_start + P.pull_time;
  if (!on) return null;
  const d = mSub(s.pos, aPt(victim, 256)), dl = mLen(d);
  if (dl < P.pull_min_dist || dl > P.pull_max_dist) return null;
  const span = Math.max(0, P.pull_full_dist - P.pull_min_dist), k = span === 0 ? 1 : Math.min(1, (dl - P.pull_min_dist) / span);
  return mMul(mNorm(d), P.pull_strength * k);
}
if (typeof module !== 'undefined') module.exports = { spawnShots, stepShot, shotContact, magiTouches, magiPull, MF, MAGI_PARAMS, setSeed: (x) => { magiSeed = x | 0; } };
```

Example (one move's projectiles against the dummy):

```js
const byId = new Map(projectiles.records.map((r) => [r.id, r]));
let live = [];                                    // on op 0x0204 at tick t: live.push(...spawnShots(byId.get(id), ownerAt, foeAt, { effect }, t))
function tickShots(t, foe) {
  const next = [];
  for (const s of live) {
    if (s.tick > t) { next.push(s); continue; }
    const o = stepShot(s, foe);
    let ev = o.events;
    if (o.hit && o.hit.fresh && magiTouches(o.hit, foe.pos)) { ev = ev.concat(shotContact(s)); /* apply atkp o.hit.atkp */ }
    const pull = magiPull(s, foe); if (pull) foe.pos = { x: foe.pos.x + pull.x, y: foe.pos.y + pull.y, z: foe.pos.z + pull.z };
    for (const e of ev) { const r = byId.get(e.fire); if (r) next.push(...spawnShots(r, ownerAt, foeAt, { parent: e.parent }, t + 1)); }
    if (o.alive && s.state <= 2) next.push(s);
  }
  live = next;
}
```

## 10. Open

* Actor +0x100 / +0x110 (aim and centre points) — the one number the studio has to guess (`MAGI_LOCK_H`).
* Whether the collision pass sweeps (H+128 → H+144) and the exact upright primitive (cylinder or capsule).
* Re-hit rules for a PIERCE bullet's hit object; `FUN_08ADA0A4` (clash / reflect destroy); which collision kind is the
  floor (1, 2 or 4); kind 4's class (`FUN_08AD7838`); the emitter's E+16 for top-level records with direction 3.
* The tick on which a child emitter first updates (same tick as the parent's event or the next: ±1 tick).
