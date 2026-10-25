# Hit contact: when an attack volume touches a fighter

Research note, 2026-10-24. Source: the static recompilation (`work/output/generated`, `work/overlays/battle/generated`,
read with `work/magi_re/fn.py`), the EBOOT image (`work/eboot_image.bin`, base 0x08804000; vtables, the clash table)
and the hit-volume, atkp and timeline sections of the 31 fighter objx files in `work/exmode_re/all/` (2,886 volume
records, 2,068 atkp records). Nothing in the game, mods, tools or studio was changed. Follows on from
[../hit-reaction/README.md](../hit-reaction/README.md) (what happens after the contact).

Labels: **confirmed** = read end to end in code; **strong** = code read, one link inferred; **guess** = inferred from
data or naming. Units: metres, 1 tick = 1/60 s.

## Summary

1. **Every volume is a sphere.** All 2,886 retail records have shape byte (+0x05) = 3, and shape 3 vs shape 3 is
   `|a − b|² < (ra + rb)²` with the centres in world space and `r` = record +0x18 (`FUN_08812D48`). No capsules
   between records: the hip and neck spheres (kind 2) and the two leg spheres (kind 3) are tested one by one.
   (confirmed)
2. **Fast swings are swept.** Each tick the attacker's hit object places every active kind-4 sphere at its current
   position and adds extra spheres of the same radius every `r` metres back toward where that record was last tick,
   until the gap left is ≤ 1.5 r (`FUN_0880BAA4`). (confirmed)
3. **The first tick of a new hit object only primes the sweep**: contacts are only emitted when the object's age
   (+0x68) > 0. So a window `{start, end}` can connect from tick start+1. (confirmed; consequence for the window
   timing strong)
4. **Kinds**: 4 = hit sphere (the only kind tested against bodies), 6 = clash/guard sphere (attack vs attack, guard),
   2 / 3 = hurt spheres (upper body / legs), 1 = body push, 5 = projectile-contact sphere (armed by
   `weapon_volume`), 7 = weapon-trail points (rendering), 10 = target point, 0 / 11 / 15 not settled. (table §3)
5. **`hit_window {hit, group}`** turns the group on (actor+0x448 mask) *and* creates/refreshes a hit object for that
   atkp + group; **`weapon_volume {group}`** only turns the group on. Retail `weapon_volume` groups hold kind 5
   (727 uses), kind 7 (465) or extra hurt/push volumes kind 1+2 (96), never kind 4. (confirmed)
6. **Order in a frame**: all actors update first (animation → timeline events → movement → bone matrices → collision
   sets), then the hit manager task (priority 31,000 vs 0 for actors) moves the hit objects and runs the contact pass
   on those fresh positions; the reaction is applied at once (moves the defender next frame). (strong)
7. **No height or facing condition** in the contact test itself. Extra gates: same owner / own summon, `invincible`
   (+0x190 bit 0x40), "already hit by this hit object" bit, collision-off status, attack classes that never hurt
   (4, 5, 9, 12–15), clash (an attack sphere touching the defender's active kind-6 sphere can cancel the hit).
   (confirmed)

---

## 1. Where the test runs

| Step | Function | What it does | Conf. |
|---|---|---|---|
| Collision sets | `FUN_08831808` (build), `FUN_08831994` → `FUN_0883E79C` (per tick) | One set per kind: header 12 B `{u32 actor, u16 count, u8 kind, u8 nrec, u32 rec*[]}` + 28-byte elements. Each tick the set is refilled with the records of that kind whose group is active (`FUN_08837D18`), each placed by `FUN_08812664`. Called at the end of the actor update `FUN_0883C2E8` (after `FUN_0886AC98` bone matrices) and again by `FUN_08B2D524` when the actor scale ≠ 1. | confirmed |
| Set offsets | actor+0x468 kind 1, +0x46C 2, +0x470 3, +0x474 6, +0x478 5, +0x47C 0xD, +0x480 0xE, +0x484 0, +0x488 8, +0x48C 4 | (kind 1 at +0x468 was missing from the earlier list) | confirmed |
| Element | `FUN_08812664` | +0x00 shape (record +0x05), +0x04 r1 (record +0x18), +0x08 r2 (record +0x1C), +0x0C record*, +0x10 centre = M(bone)·(off, 1), then centre.y −= record +0x14 (always 0 in retail) | confirmed |
| Bone matrix | `FUN_088369A4` | bone 0x4000 = actor matrix (actor+0x00); other bones with bit 0x4000 = identity (offset is a world position); else model bone matrix (`FUN_0886B590`) × actor+0x330 | confirmed |
| Hit manager | `FUN_08807538` (task registered by `FUN_08808424`, priority 31,000; actor update task `FUN_088413D0` priority 0; `FUN_08882790` keeps the list sorted ascending) | Clears the contact list (0x089D2808, max 250 entries), calls each hit object's update (vtable +0x28), age +0x68 += 1, then `FUN_08809228` → `FUN_08809CEC(list, defender)` for each actor in the three actor lists | confirmed (ascending = earlier: strong) |
| Melee hit object | vtable 0x089C6D18 (+0x28 = `FUN_0880BAA4` update, +0x30 = `FUN_0880B9C4` hit) built by `FUN_0880B8E4`; +0x80 group, +0x24 atkp id, +0x34 atkp*, +0x18 owner | | confirmed |
| Created by | `hit_window` 0x0200 → `FUN_08833B80` (mask bit + fighter vtable +0xB8 = `FUN_08833B2C` → `FUN_0883E108`) | 8 slots per actor (actor+0x3AC). Same atkp and group as an existing object → the object is kept (its already-hit bits and sweep history survive); otherwise the old one is destroyed and a new one made. | confirmed |
| Destroyed | `FUN_0880BAA4` end | when no record of its group is active any more (window over) → `FUN_08808214` | confirmed |

### Group on/off (`FUN_08837D18`, 64-bit masks, bit = group)
Checked in this order: actor+0x458 set → on; +0x460 → off; **+0x448 → on** (`hit_window`, `weapon_volume`
0x0112 handler at 0x088299E8, prop op 0x025E); +0x450 → off (ranged op **0x014E**, handler 0x08829A20; no retail
use); else the default mask in the volume section header (`u32 1, u32 0` = group 0 only). So group 0 (body, hurt,
push volumes) is always on. (confirmed; where +0x448 is cleared each tick was not found: open, but windows do end)

## 2. The geometric test (`FUN_08812D48`)

| Shapes (A, B) | Function | Test | Conf. |
|---|---|---|---|
| 3, 3 | inline | `|B.c − A.c|² < (A.r1 + B.r1)²` (3D, strict) | confirmed |
| 0/3 with 0/3 (not 3,3) | `FUN_08812054` | ellipsoid: d = B.c − A.c, `(dx² + dz²)/(A.r1+B.r1)² + dy²/(A.r2+B.r2)² < 1` | confirmed |
| 0/3 with 1 | `FUN_088121B0` | vertical cylinder B (radius r1, half-height r2) vs A: `|dy| < A.r2 + B.r2`; R = A.r1 + B.r1 if `|dy| ≤ B.r2`, else `A.r1·sqrt(1 − ((|dy| − B.r2)/A.r2)²) + B.r1`; then horizontal `dx² + dz² < R²` | confirmed |
| 1, 1 | `FUN_08812300` | `|dy| < A.r2 + B.r2` and `dx² + dz² < (A.r1 + B.r1)²` | confirmed |
| other | — | no contact | confirmed |

Retail fighters only use shape 3 with r1 = r2, so the second radius never matters. **No scale** is applied to the
radius. The actor scale (actor+0x110C) multiplies the rows of the actor matrix (`FUN_08B2D524`), so root (0x4000)
offsets scale with the fighter; whether actor+0x330 carries the scale for bone volumes was not checked. (radius:
confirmed; offsets: strong)

## 3. Kinds

Retail counts from the 31 objx (radius min / median / max).

| Kind | n | r | Used by | Meaning | Conf. |
|---|---|---|---|---|---|
| 4 | 1159 | 0.3 / 1.0 / 24 | melee hit object (`FUN_0880BAA4`, kind mask 0x50 = kinds 4 and 6, kind 6 skipped) | **hit sphere**: the only kind put in the contact list | confirmed |
| 6 | 871 | 0.45 / 1.4 / 6.5 | defender's set +0x474 in `FUN_08809F00` | **clash / guard sphere**: an attacker's kind-4 sphere touching it starts the clash rules (§5). Usually a slightly larger copy of the kind-4 spheres in the same group; `GUARD` (class 4) windows hold only a kind-6 sphere (Bartz: root (0, 0.85, 0) r 1.1). | confirmed (code), names strong |
| 2 | 66 | 0.25 / 0.3 / 0.9 | set +0x46C, contact pass | **upper hurt spheres** (Bartz: bone 3 +0.1 x, bone 0x27, r 0.3) | confirmed |
| 3 | 66 | 0.23 / 0.3 / 0.65 | set +0x470, contact pass | **leg hurt spheres** (Bartz bones 6, 0xB); a contact only here sets hit-record bit 1 | confirmed |
| 1 | 46 | 0.5 / 0.6 / 1.5 | `FUN_08833360` → `FUN_0883E904` (fighter vs fighter, kind 1 vs kind 1, push vector `FUN_08812EF0`); first active kind-1 centre is also stored at actor+0x100 | **body push** (Bartz root (0, 0.9, 0) r 0.6) | confirmed |
| 5 | 468 | 0.2 / 0.8 / 5.15 | set +0x478, read by the projectile system (`FUN_088170C0`, `FUN_08816EB8`, `FUN_08817518`: a projectile actor's kind-2 set vs fighters' and props' kind-5 sets) | **projectile contact** (melee breaking / reflecting shots?); armed by `weapon_volume` | set use confirmed, meaning guess |
| 7 | 87 | 0.1 / 0.6 / 3 | `FUN_0884BB48` (fighter and summon constructors) → GU drawing `FUN_0884BF0C` | **weapon-trail points** (rendering only); armed by `weapon_volume` | strong |
| 10 | 30 | 0.3–1.3 | `FUN_08835418` → actor+0x110 (falls back to the kind-1 point) | target / lock-on point | strong |
| 0 | 31 | 0.9–1.4 | set +0x484 (many readers, not followed) | stage collision? | guess |
| 11, 15 | 31 each | | `FUN_08835378`, `FUN_088353C8` accessors | not followed | open |

Group make-up in retail: (4, 6) 396 groups, (5) 215, (4) 88, (7) 67, (6) 53, (4, 5, 6) 51, (5, 6) 31, (1) 9,
(1, 2) 2. `hit_window` groups contain (4, 6) 1015×, (4) 390×, (6) 117× (guards / clash-only), (4, 5, 6) 50×.

## 4. Sweep and timing (`FUN_0880BAA4`)

For each record of the armed group with kind 4 or 6 (index i counts both, max 32 swept):
```
place element (FUN_08812664)               // current centre c, radius r
if age > 0: push(c)                        // contact list entry
if i < 32 and the object has history (flag 0x04):
    d = prev[i] − c;  L = |d|;  u = d / L; step = u · r
    dir += u                               // for the swing direction below
    p = c; rem = L
    while rem > 1.5 r:  p += step; if age > 0: push(p); rem −= r
prev[i] = c
```
(kind 6 records only take a slot; they are not placed or pushed.) After the loop: hit object +0x00 = −normalize(dir)
= the swing direction (hit record +0x10, used by the clash angle gate and reaction 20); if atkp flags & 0x04 it is
the attacker's forward instead. If no record was active the object is destroyed; else flag 0x04 is set. (confirmed)

Timing: actors run before the hit manager (§1), so the spheres use this tick's pose after this tick's movement, and
the defender's hurt spheres are also this tick's. A window starting at tick s creates the object at s (age 0: only
`prev[]` recorded) and can hit from s+1 to the last tick of the range; when the range ends the group goes off and the
object dies. Retail `hit_window` ranges are 4 ticks or longer (215 × 4, 336 × 6, 242 × 8, …), none 1–3. (code
confirmed; tick-exact window edges strong)

## 5. The contact pass (`FUN_08809CEC(list, defender)`)

Skipped entirely when `FUN_08837990(defender)` (collision-off status, set with the mask clear by `FUN_08837CAC`).
Then for each contact-list entry `{+4 hit object, +8 element}`, in list order:

| # | Check | Function | Conf. |
|---|---|---|---|
| 1 | skip if the object's owner is the defender, or owner/defender are summon and master | `FUN_08809110` | confirmed |
| 2 | skip if the object is cancelled (flag 0x08, set by a clash) | inline | confirmed |
| 3 | skip if melee object and owner+0x380 bit 0x80000 (owner collision off, `FUN_0883916C`) | `FUN_08808320` | confirmed |
| 4 | **may hit**: defender not `invincible` (+0x190 bit 0x40, op 0x0006); defender's "already hit by this object" bit clear (bitset at defender+0x3AC → +0x2C, index = object +0x26); attacker and defender (after summon → master, kind-1 actor → +0x1108) differ | `FUN_08807D4C`, `FUN_088378C4`, `FUN_0880AC84` | confirmed |
| 5 | **clash**: the element vs the defender's kind-6 set (+0x474); see below. A clash outcome ≠ 0 for the attacker skips the hurt test for this entry | `FUN_08809F00` | confirmed |
| 6 | skip if atkp class ∈ {4, 5, 9, 12, 13, 14, 15} (guards, `FACT_*`, EX/BLOCK classes never hurt) | `FUN_088081B8` | confirmed (class names from data) |
| 7 | defender must have a non-empty kind-2 set and status bit 0x089C7FB0 | `FUN_08837764` | confirmed |
| 8 | test kind-3 set, then kind-2 set (first element that touches, `FUN_0883EA64`). Kind 2 touched → `FUN_08808EE4(entry, def, elem, 0)`; else kind 3 touched → `FUN_088090F4` = same with flag 1 | inline | confirmed |

Then `FUN_08808EE4` (hit-reaction note §1): invulnerable status unless atkp flags 0x20, vtable refusals, the per-group
rehit timer, the hit record, the reaction. Once a sphere of an object hits, the object's later spheres find the
already-hit bit set, so one object hits a defender once per window (unless `rehit`). (strong)

### Clash (`FUN_08809F00`; not needed for a passive dummy)
- The defender must have an active hit object for the touched kind-6 record's group (`FUN_08808A74`).
- Angle gate `FUN_088092E4(X, Y)`: a = −X.swing (y = 0, falls back to −X.owner forward), b = Y.swing (falls back to
  Y.owner forward); pass if `Y.atkp[+0x0E]° ≤ |angle(b, a)| ≤ (Y.atkp[+0x0E] + Y.atkp[+0x0F])°`. Projectile
  objects (type 2) always pass. Retail: normal attacks (0, 180) = always, guards (0, 90), `FACT_*` (0, 90) or (90, 90).
- Clash if both gates pass, or only the attacker's gate when the defender's class is a guard-type class.
- Outcome = table at 0x089D4978 (`FUN_0884A454`), `T[attacker class][defender class]`, and the reverse for the
  defender; 1 + 1 becomes 5 + 5. Class modifiers: ability 0x137 turns class 9 into 4; status bit 0x40000000 or the
  EX-burst owner turns 0/1 into 2 (and 7/8 into 3 for the burst). A class-4 side whose owner has ability 0x40
  (+0x195 bit 0x20 set, +0x198 bit 0x08 clear) is then treated as class 12, its object gets flag 0x200 and
  `FUN_08B34F9C` runs (guard ability; the attacker outcome is forced non-zero unless the table gave 0 or 3).
  Outcome 4 on either side still delivers the hit (`FUN_08808EE4` with this entry: guard crushed?); outcome 3 on
  either side calls `FUN_088593B4(…, owner, 2)` (guard event). Codes 1/2/3/5/6 set the attacker object's cancel
  flag 0x08.

Rows = attacker class, columns = defender class (from the EBOOT image):
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
11:  0  ...all 0
12:  3  3  3  3  0  0  0  3  3  3  0  0  0  0  0  0
13:  3  3  1  3  0  0  0  3  3  3  0  0  0  0  0  0
14:  3  3  1  0  0  0  0  0  0  3  0  0  0  0  0  0
15:  0  0  0  1  0  0  0  3  3  0  0  0  0  0  0  0
```
Retail classes (atkp +0x04): 0 Brave melee (630), 2 HP / LIMIT (542), 3 (207), 7 (205), 1 (115), 9 `FACT_*` (93),
8 (80), 11 (72), 4 `GUARD` (41), 12 EX (34), 6 dash (22), 10, 5, 13 `BLOCK`, 14/15 guard variants. Outcome meanings
beyond "≠ 0 cancels the hit": guess.

## 6. Checks against retail data (Bartz fiv100)

- Hurt: kind 2 on bones 3 (hip, +0.1 x) and 0x27 (neck), kind 3 on bones 6 and 0xB, all r 0.3; body push kind 1
  root (0, 0.9, 0) r 0.6.
- NEAR (motion 45, ticks 10–16, group 32): kind 4 on bone 0x1D at x 0.5 and 1.0, r 0.7, plus kind 6 x 0.7 r 1.2. A
  sword sphere touches the neck when the centres are < 1.0 m apart. `weapon_volume {33}` (10–34) arms a kind-7 trail
  point at x 0.6 on bone 0x31.
- AIR_MIDDLE1_1 (motion 52, 22–36, group 42): one root sphere (0, 0.95, 0.82) r 1.0 + kind 6 r 1.3: a body-attack
  volume 0.82 m in front at chest height.
- HP_A_BLADE1_2 (motion 69, 40–54, group 34): eight kind-4 spheres r 1.0 along bone 0x19 from x 1.0 to 9.0 (a 9 m
  blade), matching kind 6 r 1.3, and `weapon_volume {35}` arms eight kind-5 spheres r 0.8 on the same line
  (projectile contact).
- GUARD (motions 20/21, 0–36, group 4): only a kind-6 root sphere (0, 0.85, 0) r 1.1, class 4, angles (0, 90).

## 7. Not settled
- Where the +0x448 "on" mask is cleared each tick, and which ops write +0x458 / +0x460.
- Whether actor+0x330 includes the actor scale (bone volumes on scaled fighters).
- Kinds 0, 11, 15 and the exact gameplay of kind 5; outcome codes of the clash table.
- Projectile hit objects (`FUN_0880B194`) use their own shape/radius (object +0xB0/+0xB4/+0xB8) and a 1.5 r sweep
  from +0x90 to +0x80; only the melee path was read in detail.

## 8. JS for the studio

Matches the melee path for a passive dummy (no clash, no guard). Vectors `{x, y, z}`, Y up, metres. Matrices are
4×4 row-major with the translation in row 3 (PSP layout: `p' = p·M`).

```js
const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y, z: a.z + b.z });
const mul = (a, k) => ({ x: a.x * k, y: a.y * k, z: a.z * k });
const dot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const xform = (m, p) => ({ x: p.x*m[0] + p.y*m[4] + p.z*m[8]  + m[12],
                           y: p.x*m[1] + p.y*m[5] + p.z*m[9]  + m[13],
                           z: p.x*m[2] + p.y*m[6] + p.z*m[10] + m[14] });

// record: {group, kind, shape, bone, off:[x,y,z], yoff, r1, r2} (objx section type 5)
// actor:  {matrix (root, already scaled), modelMatrix (+0x330), boneMatrix(i) (model space)}
function placeVolume(rec, actor) {                       // FUN_08812664 + FUN_088369A4
  const p = { x: rec.off[0], y: rec.off[1], z: rec.off[2] };
  let c;
  if (rec.bone === 0x4000) c = xform(actor.matrix, p);
  else if (rec.bone & 0x4000) c = p;                      // world position
  else c = xform(actor.modelMatrix, xform(actor.boneMatrix(rec.bone), p));
  c.y -= rec.yoff || 0;
  return { shape: rec.shape, r1: rec.r1, r2: rec.r2, c, rec };
}

function touches(a, b) {                                  // FUN_08812D48
  const d = sub(b.c, a.c);
  const s = new Set([a.shape, b.shape]);
  if (a.shape === 3 && b.shape === 3) return dot(d, d) < (a.r1 + b.r1) ** 2;
  if (!s.has(1)) {                                        // ellipsoid (shapes 0/3)
    const h = a.r1 + b.r1, v = a.r2 + b.r2;
    return (d.x * d.x + d.z * d.z) / (h * h) + (d.y * d.y) / (v * v) < 1;
  }
  if (a.shape === 1 && b.shape === 1) {
    return Math.abs(d.y) < a.r2 + b.r2 && d.x * d.x + d.z * d.z < (a.r1 + b.r1) ** 2;
  }
  const [e, cyl] = a.shape === 1 ? [b, a] : [a, b];       // ellipsoid e vs vertical cylinder cyl
  const dy = Math.abs(d.y);
  if (!(dy < e.r2 + cyl.r2)) return false;
  const R = dy <= cyl.r2 ? e.r1 + cyl.r1
          : e.r1 * Math.sqrt(1 - ((dy - cyl.r2) / e.r2) ** 2) + cyl.r1;
  return d.x * d.x + d.z * d.z < R * R;
}

// One hit object = one {atkp, group} window. Keep it across ticks while atkp and group stay the same.
function makeHitObject(atkp, group) { return { atkp, group, age: 0, prev: [], hasPrev: false, hit: new Set() }; }

// attackerVolumes: placed volumes of the attacker's active records of this group (kind 4 and 6, file order).
// Returns the contact-list spheres for this tick (FUN_0880BAA4) and updates the sweep history.
function attackSpheres(obj, attackerVolumes) {
  const out = [];
  attackerVolumes.forEach((v, i) => {
    if (v.rec.kind === 6) return;                         // takes slot i, never tested against bodies
    if (obj.age > 0) out.push(v);
    if (i < 32 && obj.hasPrev && obj.prev[i]) {
      const d = sub(obj.prev[i], v.c), L = Math.sqrt(dot(d, d));
      if (L > 0) {
        const step = mul(d, v.r1 / L);
        let p = v.c, rem = L;
        while (rem > 1.5 * v.r1) { p = add(p, step); if (obj.age > 0) out.push({ ...v, c: p }); rem -= v.r1; }
      }
    }
    obj.prev[i] = v.c;
  });
  obj.hasPrev = attackerVolumes.length > 0;
  obj.age += 1;                                           // FUN_08807538, after the update
  return out;
}

// defenderVolumes: placed volumes of the defender's active records (group 0 + any armed groups).
// Returns null or {sphere, hurt, legs} for the first sphere that connects (FUN_08809CEC order).
function contact(obj, spheres, defenderVolumes, defender) {
  if (defender.invincible || defender.collisionOff) return null;
  if (obj.hit.has(defender.id)) return null;               // already hit by this object (rehit clears it)
  if ([4, 5, 9, 12, 13, 14, 15].includes(obj.atkp.class)) return null;
  const legs = defenderVolumes.filter(v => v.rec.kind === 3);
  const body = defenderVolumes.filter(v => v.rec.kind === 2);
  if (!body.length) return null;
  for (const s of spheres) {
    const l = legs.find(v => touches(v, s));
    const b = body.find(v => touches(v, s));
    if (b || l) { obj.hit.add(defender.id); return { sphere: s, hurt: b || l, legs: !b }; }
  }
  return null;
}
```
Per studio tick: advance the attacker's animation and movement, update bone matrices, rebuild both fighters' placed
volumes, then for each armed `hit_window` call `attackSpheres` and `contact`, then apply the reaction
(hit-reaction note §8). A window that ends destroys its object; the next window with a different atkp (or group)
gets a fresh object and can hit again. `rehit` N: clear `obj.hit` for that defender N ticks after a hit.
