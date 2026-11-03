# atkp open questions: the unknown fields of the hit record

Research note, 2026-10-31. Source: the static recompilation (`work/output/generated`, `work/overlays/battle/generated`,
read with `work/magi_re/fn.py`) and the 2,060 atkp records of the 31 retail fighter objx files in
`work/exmode_re/all/` (decoded with `charmod_objx.hit_decode`). Nothing in the game, mods, tools or studio was changed.
Follows on from hit-reaction/, hit-contact/ and guard-dodge-walls/.

Labels: **confirmed** = read end to end in code; **strong** = one link inferred; **guess** = from data or naming.

Record size: the lookup (`FUN_08837F14` / fallback `FUN_0880BE84`, binary search via `FUN_08934944`) uses 0x44-byte
records: 0x34 bytes of fields + a 16-byte name at +0x34. Actor kinds used below (actor +0x542): 0 = main fighter,
1 = assist, 3 = summon / thrown object (owner at +0x5D4) (event-ops/K.md).

## Summary

| Off | Studio name | Answer | Conf. |
|---|---|---|---|
| +0x02 u16 | unk_02 | No reader found. Retail: 0 in all 2,060 records. Probably padding. | guess (no reader) |
| +0x04 | class | Clash table row/column (`FUN_08809F00`, table 0x089D4978), never-hurt test (`FUN_088081B8`: 4, 5, 9, 12–15), CPU AI copies the clash logic (`FUN_08ADA0A4`, `FUN_08AD1968` → `FUN_08ADB330`). Tooltip is right. | confirmed |
| +0x06 u8 | unk_06 | **Second contact-effect bit**: the hit spawns effects twice, pass 0 with +0x0C, pass 1 with +0x06 (0xFF = none). | confirmed |
| +0x07 s8 | unk_07 | **Critical-hit chance bonus** (signed, added to the base crit rate in the Brave damage calc). Retail: 0 everywhere. | strong |
| +0x0D u8 | hitstop | **Not hit-stop.** Number of particles ("orbs") a hit scatters, ×(value/10). Probably the EX Force released by the hit. | code confirmed, "EX Force" guess |
| +0x0E / +0x0F | unk_0e / unk_0f | Clash angle gate: pass if `+0x0E ≤ angle ≤ +0x0E + +0x0F` (degrees). | confirmed |
| +0x12 | rehit | Read as a **u8** (`FUN_0880AAE0`), not u16. | confirmed |
| +0x13 u8 | (high byte of rehit) | Only read when a summon's atkp is built from two records (summed). Retail 0. | confirmed (reader), meaning open |
| +0x14 u8 | shake byte 0 | **This is the hit-stop**: game speed ×0.15 (or frozen) for N ticks. | confirmed |
| +0x2C / +0x2E | kb_bone / unk_2e | +0x2C is an s16 bone for reactions 11–15 and a **float** for reactions 2, 3, 17. unk_2e is just the high half of that float. | confirmed |
| +0x08 flags | flags | 0x01 random power, 0x02 HP, 0x04 swing dir, 0x10 HP-hit side flag, 0x20 ignore invuln, 0x40 / 0x200 / 0x400 damage-calc modifiers, 0x80 attack category (crit/ability set), 0x100 extras, 0x800 effect aim, 0x2000 assist AI. See §6. | mixed |

---

## 1. +0x04 class

Readers:
- `FUN_08809F00` (clash): `cA = X.atkp+4`, `cD = Y.atkp+4`, adjusted (ability 0x137: 9 → 4; `+0x380` bit 0x40000000:
  0/1 → 2; EX-burst owner: 0/1 → 2, 7/8 → 3; perfect guard: 4 → 12), outcome `T[cA][cD]` from 0x089D4978
  (guard-dodge-walls §1.2, §2). (confirmed)
- `FUN_088081B8`: classes 4, 5, 9, 12, 13, 14, 15 never reach the hurt test. (confirmed)
- CPU AI: `FUN_08ADA0A4` reads the opponent hit object's class with the same adjustments (0x40000000 → 2, EX-burst owner
  → 2/3; hit-object flag 0x200 → 12) and `FUN_08AD1968` passes both classes to `FUN_08ADB330` (the AI's own clash
  lookup). Other class readers in the overlay (`FUN_08A9BAE0`, `FUN_08A40730`, `FUN_08A23DA8`, `FUN_08817518`) were
  not followed. (strong)

Retail counts (2,060): 0 630, 2 542, 3 207, 7 205, 1 115, 9 93, 8 80, 11 72, 4 41, 12 34, 6 22, 10 10, 5 4, 13–15 1–2.
What each class does against a guard is in guard-dodge-walls §2 ("Guard (class 4) against each attacker class").

Tooltip check: "decides who wins a clash. 0 Brave melee, 2 HP / limit; 4,5,9,12–15 never hurt" — **correct**. Two
refinements worth stating: class 2 and 3 crush a guard, 0 and 9 are blocked, 7 is a projectile class, 11 never clashes.

## 2. +0x0D: orb / EX Force amount (not hit-stop)

`FUN_08808C98` (called on a fresh hit, after the contact effects):
- attacker = hit object +0x18 (kind 3 → owner +0x5D4). **Skipped only when the attacker is an assist (kind 1)**; main
  fighters (kind 0), summons' owners and ownerless objects go on. The old note "fighters never read it" was wrong.
- position = the hit record's contact point (+0x10..+0x18 of the argument), w = 1.
- HP hit (`FUN_088132BC`) → `FUN_08ABB690(mgr, attacker, pos, atkp+0x0D)`, Brave hit → `FUN_08ABB60C(...)`, mgr =
  *(0x08ABBDF8). (confirmed)

`FUN_08ABB60C` / `FUN_08ABB690` → `FUN_08ABD6F4` / `FUN_08ABD810` (mgr+0x15C):
- nothing if the attacker has ability **0xEB** (`FUN_08ABDDD0`).
- count = int( table[11].+0x48 (Brave) or table[12] (HP, via `FUN_08ABDC60`) × (+0x0D / 10) × (attacker ? 1 : 0) )
  (`FUN_08ABDBB4`, `FUN_08ABDD94`, `FUN_08ABDDB0`).
- `FUN_08ABFA4C` spawns that many particles at the contact point. (confirmed)
- Particles are later collected: `FUN_08ABF104` adds 1 per particle to a u16 counter `+0x10CA` on the collector
  (`FUN_08B333A4`), which `FUN_08AEC338` reads. (confirmed)

The same manager also draws the clash spark (`FUN_08ABB714` from `FUN_08809F00`). In Dissidia, landing Brave
attacks scatters EX Force, so this is very likely the EX Force release; not proven which gauge the counter feeds.
(guess)

Retail +0x0D: 0 (1,038), 20 (318), 1 (232), 2 (146), 5 (107), 3 (102), 10 (59), 4, 6, 7, 15.

## 3. +0x14 byte 0: the hit-stop

`FUN_08807E6C` (fresh hit only, arg `r8 == 1`; attacker resolved through kind 3 → owner; **skipped for assists**,
kind 1): if `atkp+0x14 > 0` → `FUN_08ACEEF4(*(0x08ACF110), FUN_088378E8(attacker), float(+0x14))`. (confirmed)

`FUN_08ACEEF4(mgr, freeze, ticks)`:
- needs mgr+0 (enabled by `FUN_08ACEEC4`, from battle setup `FUN_0888C4C8`);
- does nothing if `FUN_08ACF084` finds any actor with status bits in the masks at 0x08ACF108/0x08ACF10C
  (`+0x388`/`+0x38C`) (which statuses: open);
- speed = **0.0** if the attacker has `+0x384` bit 0x2000 (`FUN_088378E8`), else **0.15**;
- `FUN_08A62020(*(0x08A62280), 9, speed)`: slot 9 of a 10-slot time-scale stack (default 2.0 each). The product
  `2·Π(slot/2)` goes to `FUN_08864D98` → `FUN_08864CD8` (speed object *(0x089D5EA8), flag bit 0x01);
- mgr+8 = ticks, mgr+4 = 0. (confirmed)

Per tick: `FUN_08ACF064` (registered as a task in `FUN_0888C4C8`) → `FUN_08ACEFD8`: mgr+8 -= real frame step
(*(0x089D69E0)+0x18); at ≤ 0, or as soon as `FUN_08ACF084` is true, `FUN_08ACEF94` resets slot 9 → normal speed.
The game-speed task `FUN_08864DD8` writes the global speed **0x089D5EAC** = speed object +0 while it is active, else the
real frame step. Every actor clock goes through it (`FUN_08838300` = clock × 0x089D5EAC). (confirmed)

So: **+0x14 = hit-stop length in ticks; the whole game runs at 15 % speed (or stops) for that long.** It is the only
hit-stop in the hit path. Retail values: 0 (1,646), 6 (175), 8 (108), 10 (108), 12 (21), 14 (2).
What sets attacker `+0x384` bit 0x2000 (full freeze) is open.

Studio impact: "shake" is really four bytes: +0x14 hit-stop ticks, +0x15 shake ticks, +0x16 / +0x17 shake amplitudes ×0.01.

## 4. +0x06: second contact effect

`FUN_088085A8` and `FUN_08808748` loop twice over `FUN_0880966C(..., pass)`: pass 0 uses `atkp+0x0C`, pass 1 uses
`atkp+0x06` (0xFF = skip). The effect search callback (0x08808968) plays records whose bitset (+0x3C) has that bit and
whose +0x38 bit 0x01 is set. Flag **0x800** makes the effect face along the normalized vector from the hit position
(effect direction). (confirmed)

Retail: 0xFF in 2,055 records; set only in Tifa sev110 HP_L_SSKICK (24), HP_L_MIMPACT1 (16), HP_L_MSTRIKE2_2 (25),
HP_A_MRUSH2_2 (26) and Emperor two100 AR_COMBO_ABI1_6 (32).

## 5. +0x07: critical chance bonus

`FUN_08AE45B8` (Brave damage; entries of 64 bytes with +0x20 = atkp, +0x2C = attacker) calls
`FUN_08AE70E4(entry, ..., s8 atkp+0x07, attackerIsAssist, ..., flags & 0x80)`. That function sums a base rate
(*(0x08B4AF14)+0x21), stage/HP-attack bonuses and ability values (0x141, 0x06 when flag 0x80 is clear, 0x07 when it
is set, 0x30/0x5F against 0x4A/0x4B on the defender) into r19, clamps to [0, cap], adds **+0x07**, and returns
`rand % cap < (sum × k / 100)`. The caller takes the critical path on a 1 (abilities 0x6F, 0x91, 0x87/0x88 ...).
(code confirmed; "critical" is the strong part)

The summon builder `FUN_0884FE10` also adds the modifier record's +0x07 to the base record's. Retail: 0 everywhere.

## 6. Flags (+0x08)

Retail bit counts: 0x80 573, 0x02 280, 0x10 230, 0x04 140, 0x20 66, 0x400 42, 0x200 29, 0x01 12, 0x2000 4,
0x800 2, 0x1000 2, 0x40 1, 0x100 1.

| Bit | Reader | Effect | Conf. |
|---|---|---|---|
| 0x01 | `FUN_08AE45B8` | power = rand % power + 1 | confirmed |
| 0x02 | `FUN_088132BC` (many callers) | HP attack | confirmed |
| 0x04 | `FUN_0880BAA4` | alternative swing direction for the hit record +0x10 | confirmed (hit-contact) |
| 0x10 | `FUN_08AE5BC8` (HP-damage pass) | on an HP hit, unless the defender has `+0x197` bit 0x80 / `+0x188` bit 0x20, sets `+0x5B2` bit 0x10 on the side's fighter. All 230 retail uses also have 0x02. What reads `+0x5B2` 0x10 is open | strong (reader), meaning open |
| 0x20 | `FUN_08808EE4` | hits through invulnerability | confirmed |
| 0x40 | `FUN_08AE45B8` | damage modifier when attacker and defender have the same `+0x10B4` | strong |
| 0x80 | `FUN_08AE45B8`, `FUN_08AE70E4`, `FUN_08AE5BC8` | attack category: picks crit ability 0x07 instead of 0x06, power ability 0x88 instead of 0x87, and set b instead of set a of the 0x0B36 timed buff. Retail mostly classes 3, 7, 2, 11 (ranged / magic?) | strong (mechanism), name guess |
| 0x100 | `FUN_08B33938`, `FUN_08AE5BC8` | Brave/HUD extras run even if `FUN_08B34488` fails; HP pass sets side +0x2A4 | strong |
| 0x200 | `FUN_08AE45B8` | sets result bit 0x100 | strong (meaning open) |
| 0x400 | `FUN_08AE45B8`, `FUN_08AEC7D8`, `FUN_08AEDC80` | excludes the hit from the ability-0xFC proc roll | strong |
| 0x800 | `FUN_0880966C` | contact effect aimed along the hit vector | confirmed |
| 0x2000 | `FUN_08ADAD54` | an assist's hit object (hit-object flag 0x80) with this atkp flag is skipped by that routine | strong (meaning open) |
| 0x1000 | — | no reader found | open |

## 7. +0x0E / +0x0F clash angles

`FUN_088092E4(X, Y)` (re-read): a = −X swing (y = 0, normalized; hit-object type +0x2C = 2 → always pass, type 1 →
owner forward), b = Y swing (same rules), angle = `FUN_08868128(a, b)`; pass iff
`Y.atkp[+0x0E]·π/180 ≤ angle ≤ (Y.atkp[+0x0E] + Y.atkp[+0x0F])·π/180`. (confirmed)
Retail: (0, 180) = any direction (1,922); (0, 90) = front half: guards (class 4) and some class 5/6/13–15; (90, 90) =
back half, 31 class-9 records.

## 8. +0x12 / +0x13, +0x2C / +0x2E

- `FUN_0880AAE0` reads rehit as `MEM_BU(+0x12)`: rehit is a u8 (retail max < 256). +0x13 is read only by
  `FUN_0884FE10` (summon atkp = copy of base record, then +0x0C replaced, power from the modifier, +0x0D, +0x13 and
  +0x07 summed; stored by `FUN_08850FF4` at summon +0x590 with +0x60F = 1, used by `FUN_08807860`). (confirmed)
- Reactions 2, 3, 17 read +0x28 and +0x2C as floats (`FUN_0881341C`); reactions 11–15 read +0x2C as s16 bone
  (`FUN_08813484`). So unk_2e is the high half of the float when the reaction is 2/3/17; retail non-zero unk_2e
  occurs only with float values 0.01–0.6 in +0x2C. (confirmed)

## 9. Not settled

- What sets attacker `+0x384` bit 0x2000 (hit-stop freezes instead of slowing) and which statuses
  0x08ACF108/0x08ACF10C stand for.
- Whether the +0x0D particles feed the EX gauge (counter `+0x10CA`, reader `FUN_08AEC338`).
- Readers of `+0x5B2` bit 0x10 (flag 0x10), flag 0x1000, and the meaning of +0x13.

## 10. Recommended Studio labels

- unk_02 → "unused (always 0)".
- class → keep; tooltip: "Clash class: decides who wins when two attacks or an attack and a guard meet. 0 Brave melee
  (blocked by guard), 2/3 crush a guard, 7 projectile, 11 never clashes; 4, 5, 9, 12–15 never hurt."
- unk_06 → "contact effect bit 2": "second contact effect: effect records with this bit also play on hit (255 = none)".
- unk_07 → "crit bonus": "added to the critical-hit chance of this hit (signed; retail 0)".
- hitstop (+0x0D) → "EX force": "orbs scattered on hit (× value/10; probably EX Force). Not hit-stop."
- shake byte 0 (+0x14) → "hit-stop": "ticks the whole game runs at 15 % speed after this hit lands (0 = none)".
- unk_0e / unk_0f → "clash angle min / range": "clash only if the swing angle is between min and min+range degrees
  (0/180 = any, 0/90 = front only)".
- rehit → u8; +0x13 "summon modifier (unused)".
- unk_2e → fold into +0x2C: show a float for reactions 2, 3, 17 and a bone for 11–15.
