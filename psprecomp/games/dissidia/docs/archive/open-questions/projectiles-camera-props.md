# Open questions: projectile aim points, magi flags, `camr` presets, wpnp flags

Research note, 2026-10-31. Answers the "Open" items in `../projectile-flight/README.md` (§2.3, §10),
`../../spawned-objects.md` (Open) and `../../moves-and-events.md` ("Still open": camr presets). Read from the recompiled
EBOOT / battle overlay (`work/magi_re/fn.py <addr>`), the 31 fighter objx in `work/exmode_re/all/`, the retail GMOs in
`work/exmode_re/retail/` and `battle.bin` inside `general_archive/main/main.bin`. The live debug socket was not used
(no runtime was confirmed running). Nothing in the game, mods, tools or studio was changed.

Labels: **confirmed** = read in code end to end; **strong** = code read, one link inferred; **guess** = data or naming only.

## 1. Actor +0x100 and +0x110 (projectile centre / aim points)

**Writer: `FUN_08831994`** (confirmed), the per-tick collision-set refresh called at the end of the actor update
(`FUN_0883C2E8`, after the bone matrices) — the same function that refills the collision sets (see
`../hit-contact/README.md` §1). There is no direct store because both points go through the vec4 copy `FUN_089A3868`
with `r4 = actor+0x100 / +0x110`:

```
rec = FUN_08835358(actor)            // = FUN_08835174(actor, kind 1, -1): first ACTIVE kind-1 volume record
actor+0x100 = rec ? place(rec).centre : actor+0x80          // place = FUN_08812664 (bone matrix · offset, y -= rec+0x14)
rec = FUN_08835418(actor)            // = FUN_088352E8(actor, kind 10, i), i = 1 if (actor+0x196 & 0x08) else 0
actor+0x110 = rec ? place(rec).centre : actor+0x100
```

* **+0x100 = the centre of the body-push sphere (kind 1)**, normally a root-relative point `(0, y, 0)` → `y` metres
  above the feet, scaled with the actor. Retail group-0 kind-1 heights: 0.7 (gst100, thr100), 0.8 (gst110, nin100,
  ten100), **0.9** (fiv100 Bartz, gst120/130, sev100/110, six100, ten110 — and Noctis p_noc100, built on fiv100),
  0.95 (two100), 1.0 (eht*, for100/110, nin200, one100, sev200, ten200, two200, gst200), 1.1–1.5 (the large `*200`
  bodies). Median of the humanoids ≈ 0.9–1.0 m. (confirmed code, values from data)
* **+0x110 = the centre of the kind-10 "target" sphere.** In every retail objx that has one it sits on the **`waist`
  bone** at offset 0 (gst100: waist +0.3 x). Bind-pose heights of that bone: ten100 1.11, sev100/110 1.09, gst130
  1.10, gst200 1.22, eht100 1.26, for100 1.26, gst110 0.95 m (bones resolved through the GMO; animated in game, so a
  crouch or bend lowers it). The next bone (`chest`, kind 15, r 0.1) sits ≈ 0.12 m higher. (confirmed code, strong
  for "waist" = the named bone in the GMOs checked)
* **Bartz (fiv100) and Noctis (p_noc100, p_noc100_a) have no kind-10 record**, so for them +0x110 = +0x100 =
  (0, 0.9, 0) above the root. one200 has its kind-10 on bone 0x10 r 1.3; org210 has no group-0 kind-1 (its +0x100
  falls back to a bone kind-1, then to +0x80). (confirmed)
* `actor+0x196 & 0x08` asks for the *second* kind-10 record; no retail fighter has two, so with that bit +0x110 =
  +0x100. What sets the bit was not traced (open).

**For the Studio**: split `MAGI_LOCK_H` in two: centre (+0x100) **0.9 m**, aim (+0x110) **1.1 m** for a generic
fighter dummy (0.9 m if the dummy stands in for Bartz/Noctis-type data without a kind-10 record). Consequence for
Noctis: `aim_height 1` aims at ≈ 2.0–2.1 m above the foe's feet — **above the head** (head bone ≈ 1.57–1.6 m), not
the chest. Chest aim is `aim_height ≈ 0.1–0.2`, waist aim is 0. (strong: the waist moves with the animation)

## 2. Magi record flags and +0x2A

| bit | name | what the code does | conf |
|---|---|---|---|
| 0x10 | UNK_10 → **TOUCH_ONLY** | `FUN_08AD7D5C` / `FUN_08AD9CE4` (hit object creation) set hit object +0x10 bit 0x80. The bullet hit class (vtable 0x089C6CA8) has `FUN_088080C4` as its contact handler; with bit 0x80 it skips `FUN_08807D4C`/`FUN_08807E6C` (no damage, no reaction) and only sets H+0x10 bit 2 (touched). The bullet still sees the contact: `link_hit` fires and a non-PIERCE bullet dies. So: **contact without damage**. 426 retail records. | confirmed |
| 0x400 | UNK_400 | passed as the last argument of `FUN_0880B05C` → H+0x10 bit 0x100. Only bullet hit objects (+0x2C = 2) with that bit are tested by the pass `FUN_088170C0` / `FUN_08817518` (list at `*(FUN_0881BAA8())+0x118`, singleton 0x08B632A0): bullet sphere (H+0xB0/+0xB4) vs the listed actors' kind-2 sets (actor+0x46C) or a kind-5 owner's set; a hit calls `FUN_08819624(.., 1000, 1)` and sets the bullet owner's +0x384 bit 0x01000000. Meaning (stage gimmick / AI sensing of incoming shots) not settled. 548 records. | confirmed (bit), guess (meaning) |
| 0x80000000 | UNK_80000000 → **HIT_DIR_TRAVEL** | `FUN_08AD9CE4` sets H+0xC0 bit 2. Hit-direction method `FUN_0880B508` (bullet vtable slot 3): with the bit, the hit direction = normalize(H+0xA0, the hit object's direction); if that is zero, the horizontal vector bullet centre → defender. Without it the base `FUN_08807AB8` uses shooter root → defender (horizontal). So the victim is knocked **along the shot** instead of **away from the shooter**. 11 records (eht110 …). | strong (H+0xA0 = bullet dir not re-read) |
| 0x4 | GROUND_HUG | also sets H+0xC0 bit 1 (`FUN_08AD9CE4`) | confirmed |
| 0x4000000 | NO_IMPACT_FX | also skips the `FUN_0885B7F8` registration of the bullet's hit volume in `FUN_08AD9CE4` | confirmed (call skipped), guess (what the list is) |
| 0x20000000 / 0x40000000 | CANCEL_COND1/2 | `FUN_08AD1588`: the emitter stops when the **second target** (owner vtable +304) has `+0x188 & 0x10 == 0` (1) / `+0x31C ≤ 0.0` (2). The README's "owner sub-state" is the second target's. | confirmed |
| 0x1000, 0x2000000, 0x10000000 | SWEEP_ALT, COLL_MASK_ALT, SPAWN_LOS | read only in `FUN_08AD5D70` (spawn) and `FUN_08AD48E8` (terrain step) (COLL_MASK_ALT also `FUN_08AD8284`), as the README says | confirmed (readers) |
| 0x800000 | PULL_COND | read only in `FUN_08AD2CE0` | confirmed |
| 0x8 / 0x40 | FX_FOLLOW_DIR / FX_ALIGN_FLOOR | read in the visual builders `FUN_08AD20BC`, `FUN_08AD247C`, `FUN_08AD99F8` (and 0x8 in `FUN_08AD48E8`) | confirmed (readers) |
| 0x200 | NO_VISUALS | `FUN_08AD96F0` only | confirmed |

**+0x2A (unk_2a)**: 0 in all 1,223 retail records; `motion` is read as a signed halfword at +0x28 (`FUN_08AD48E8`,
`FUN_08AD2FA8`); the only `+42` halfword read in the overlay (`FUN_08ADA2F8`) is on the hit object, not the record.
Padding. (strong) — consistent with reading +0x28 as one u32.

## 3. `camr` (motion_camera 0x030C presets)

Lives in **`battle.bin` (inside `general_archive/main/main.bin`) → ARC entry `camr`**, 1,472 bytes, loaded by
`FUN_088135D4` → `FUN_08811598` (confirmed). Layout:

| off | content |
|---|---|
| +0x00 | `"CAM "`, u32 0x20100720 (a date), **u32 count = 10**, u32 0 |
| +0x10 | u32 1, f32 1.5 (the float copied to cam+0xD4 on enter) |
| +0x18 | 0x378 bytes = **6 rows × 0x94 (37 floats)**: the free / lock-on camera tuning table (distances 3..6 / 6..8, rates 0.08 / 0.9, 50 …). This is the "6 rows of 0x94" table other notes call camr; it is **not** the preset table. |
| +0x390 | **10 presets × 0x38** (`FUN_08B3E8B0`: base + idx·0x38; `FUN_08B3E840` clamps idx ≥ count to 0) |

Presets (fields as in `../event-ops/N.md` Q2: eye spring k1/k2, target spring k1/k2, blend in/out for eye, look, up,
FOV, look distance, flags):

| # | eye k | tgt k | eye in/out | look in/out | up in/out | fov in/out | look | flags | used by |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 0, 0 | 0, 0 | 0.1 / 0 | 0.1 / 0 | 0.1 / 0 | 0.1 / 0 | 1 | 0x30 | (default for out-of-range ids) |
| 1 | 0.1, 1 | 0.1, 1 | 0.1 / 0.5 | 0.5 / 0.03 | 0 / 0 | 0.01 / 0.01 | 5.5 | 0xB5 | events `{1,0,0}` ×58 (scripts 200/203, every character) |
| 2 | 0.2, 0.89 | 0, 0 | 0.1 / 0.1 | 0.1 / 0.1 | 0.1 / 0.1 | 0.1 / 0.1 | 1 | 0x138 | `{2,0,0}` ×1 (eht200 10071) |
| 3 | 0.2, 0.89 | 0, 0 | 0.1 / 0.1 | 0.1 / 0.1 | 0.1 / 0.1 | 0.1 / 0.1 | 1 | 0xBC | none found |
| 4 | 0 | 0 | 0 (cut) | 0 | 0 | 0 | 1 | 0x08 | `{4,0,0}` ×1 (Shantotto 8500) |
| 5 | 0 | 0 | 0 (cut) | 0 | 0 | 0 | 1 | 0x18 | `{5,0,0}` ×177 + `{5,1,0}` ×15 (EX Bursts); code: `FUN_08B3AE14(…, 5)` from the EX-mode overlay code 0x08A2B98C, 0x08A2F93C, 0x08A36EE4, 0x08A3878C, 0x08A3B834, 0x08A48934, 0x08A4B234 |
| 6 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | code: `FUN_08B0A090` |
| 7 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | code: `FUN_08B10210` |
| 8 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0x08 | none found |
| 9 | 0 | 0 | 0.1 / 0 | 0.1 / 0 | 0.1 / 0 | 0.1 / 0 | 1 | 0 | none found |

Reading the flags (meanings from N.md, confirmed there except 0x10/0x100): preset 5 (EX Burst) = hard cut in and
out, bone up vector kept (0x08), post step (0x10), camera owner keeps animating during time-stop (0x20 clear), foe
**not** frozen by the camera (no 0x40). Preset 1 (pose) = spring-smoothed (0x01), looks at the actor itself (0x04),
5.5 m look distance unused, owner does not keep updating (0x20), soft blends. Flag **0x80** (presets 1, 3) is not
read by enter/exit/update/smoothing (`FUN_08B3E404`, `E580`, `E6D0`, `E954`, `E888`, `E8D0`) — unknown, probably
unused. (table confirmed; usage counts from N.md; code callers confirmed)

## 4. wpnp flags and Gilgamesh's weapon roll

Row layout corrections (confirmed): **+0x04 is u8 charId, +0x05 u8 "combo mask"** (the "odd" charIds 1817 / 281 / 793 /
1305 of rows 24–31 are 0x19 = 25 Gilgamesh with mask 7 / 1 / 3 / 5); **+0x28 u8 / +0x29 u8 = this row's weight in the
random weapon draw** (normal / when the owner is an assist); the alternate objentry choice is `owner+0x2F8 → +8`:
bit 0x01 manikin, bit 0x08 assist (+0x2A / +0x2C / +0x2E as documented). (`FUN_0884FA84`, `FUN_088508D0`)

| flag (+0x06) | effect | conf |
|---|---|---|
| 0x0020 | at spawn (`FUN_0884FA84`, and row change `FUN_0884EDE4`) clears slot +0x18 (default 1 from `FUN_0884ECD4`). In slot state 1 (set by `FUN_0884EDD0` right after spawn) the manager `FUN_088501D8` calls `FUN_0884EEF4` (→ state 2) only while +0x18 is set. So 0x20 keeps the slot in state 1 instead of moving straight to state 2. Used only by placeholder rows 24–31 (flags 0x30). Meaning of the states not settled. | confirmed (mechanism), guess (meaning: "stay held until released") |
| 0x0080 | **marks the per-character roll row** (row 23, Gilgamesh, no object). `FUN_08850574` looks up the owner's first 0x80 row when a reroll is pending (mgr+0x208). | confirmed |
| 0x0100 | sets object +0x614 = 1 at spawn. The hit-object constructor `FUN_08807860` then keeps the **object itself as the attacker**; without it the hit is credited to the object's owner fighter (+0x5D4). Yuna rows 32–76 (summon scripts 10xxx), Bartz row 82. | strong |
| 0x0001 / 0x0002 / 0x0004 / 0x0008 | rows 16, 21, 19, 22 (Gilgamesh) — no reader found in the spawn path | open |

**The roll** (confirmed, `FUN_0884F5D8` arms it: mgr+0x208 = 1, mgr+0x204 = 0):

1. `FUN_08850574` (on the next spawn while +0x208 is set): `r = rand() % 101` (LCG word 0x09AF84D0, same constants as
   magi). Walk the roll row's +0x38 weights `[45, 23, 22, 0]` with a running sum; the first k with `r < sum` gives
   **mgr+0x204 = k + 1** (combo 1: 45 %, 2: 23 %, 3: 22 %; r ≥ 90 → 0, about 10 %). If a combo was picked, draw two
   weapons: **+0x205 = A**, **+0x206 = B** with `FUN_08850A64`. Then +0x208 = 0.
2. `FUN_08850A64` → `FUN_088508D0`: a new `rand() % 101` against the running sum of +0x28 (or +0x29 for an assist)
   over all rows of the owner's charId; returns the row index. Gilgamesh rows 15–22 weights 10, 10, 15, 10, 10, 15, 15,
   15 (= 100; r = 100 falls through → 0); as an assist only rows 15/16 (50 / 50). Yuna rows 72–76 carry 20 each.
3. Spawn with a placeholder row (objentry −1, rows 23–31) in `FUN_0884FA84`: if mgr+0x204 = 0, draw a fresh weapon for
   this slot; else test the placeholder row's +0x05 mask bit (1 << (combo − 1)): set → weapon A (+0x205), clear →
   weapon B (+0x206). Rows 24/28 (mask 7) take A in combos 1–3, rows 25/29 (mask 1) only in combo 1, 26/30 (mask 3)
   in 1–2, 27/31 (mask 5) in 1 and 3.
4. Event arg2 ≠ 0 uses **+0x207** = `FUN_0884F90C`: a fresh weighted draw while the reroll is pending, else the last one.

So in the Studio a Gilgamesh event naming row 15 with arg2 = 0 shows row 15; arg2 ≠ 0 or a placeholder row is a
weighted random weapon (Studio can show the most likely, rows 17/20/21/22 at 15 %, or let the user pick).

## Still open

* What sets `actor+0x196 & 0x08` (second kind-10 point); kind 11 / 15 volumes.
* UNK_400's purpose (the 0x08B632A0 list and `FUN_08819624`); the `FUN_0885B7F8` list.
* camr flag 0x80; the 6 × 0x94 tuning rows field by field.
* wpnp slot states 1/2 and the meaning of row flags 0x01–0x08 / 0x10.
