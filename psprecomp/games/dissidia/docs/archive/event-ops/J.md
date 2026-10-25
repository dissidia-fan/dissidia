# Agent J: ranged ops 0x025E, 0x055F, 0x045B, 0x0360, 0x0D64, 0x0D63, 0x047A, 0x003E, 0x0340, 0x002F, 0x0524

**Summary**
- **Update 25 Oct:** the wpnp rows are read statically now, and the prop id is the owner's bone it hangs on (37 `l_wep`, 61 `r_wep`): [spawned-objects.md](../../spawned-objects.md).
- **Gilgamesh's weapon ops.** 0x025E, 0x045B, 0x055F and 0x0360 are used almost only by **fiv210 = Gilgamesh**. They act on his spawned weapon props: hit volume (0x025E), damage hit (0x045B), trail (0x055F), fade (0x0360). The props sit in a 16-slot child-object list at actor+0xD64 and are found by **prop id** (37/61 = the two normal hands; 23/49, 25/51, 27/53 appear only in EX-Mode-only events, `pad` 512).
  - 0x045B also builds a per-weapon damage profile, with optional random or HP-based power changes. Gilgamesh has hundreds of these events because every swing repeats them once per weapon id.
- **Bone aim, fade and evade.** 0x0D64/0x0D63 = **bone aim (look-at)**: up to two bones (spine/arms) twist toward the target or opponent within angle limits, with an optional player-steered mode. 0x0340 = **fade the fighter to X% opacity** for the range, then fade back. 0x002F = **counts as evading** (same as being in a dodge/guard script).
- **Cancels, EX Burst, mash-to-fire.** 0x047A = **"can act" that applies only on hit (arg2=0) or only on whiff (arg2=1)**. 0x003E = **EX Burst: release the burst's position lock** on attacker and victim. 0x0524 = **mash ○/□ to fire extra projectiles** (magi id, cooldown).

Common facts used below:
- Dispatcher regs: s0 = event, s1 = motion player (actor+0x130, so s1+0x60 = actor+0x190), s2 = actor. Args start at event+0xC.
- `pad` = event bytes 6/7. Walker FUN_08828EEC filters on them:
  - byte 7 (pad>>8): 1 = only outside EX Mode, 2 = only in EX Mode. FUN_0882F30C passes 2 when actor+0x5B2 bit 0x04 is set.
  - byte 6 = phase filter.
- FUN_0883739C(actor) = "is a fighter" (kind 0/1). Many handlers below do nothing for other objects.

---

## The child-prop list at actor+0xD64 (shared by 0x025E / 0x045B / 0x055F / 0x0360)
- 16 entries × 0x20 bytes. Fields:
  - +0x00 = list base (its +0x200 = the owner actor)
  - +0x04 = prop definition (a row of the global table at *0x089D4DA8, i.e. the wpnp row)
  - +0x08 = the prop object (an actor-like object with its own motion player at +0x130, alpha at +0x3B0, hit-volume mask at +0x448/+0x44C)
  - +0x10 = s16 **prop id**; +0x12 = state (0 free, 1 shown, 2–5 despawning); +0x1B = active
  - +0x14/+0x16 = fade-in / fade-out ticks; +0x19/+0x1A = fade requests; +0x1C/+0x1D = custom-hit built / armed
- Lookups:
  - FUN_0884F6F4(list, id) returns the object.
  - FUN_0884EDC0(entry, id) = `entry+0x10 == id`.
  - FUN_0884F630 = count of active entries.
  - FUN_0884F67C(list, i) / FUN_0884F6B8(list, i) = id / object of entry i.
- Props are spawned by:
  - ranged code 0x5A (`0x075A`, 0x0882C570): `{id, wpnp row, …}` → FUN_08850574;
  - code 0x65 (`0x0C65`, 0x0882C668), the summon-object variant;
  - overlay code: FUN_08A4C4C8 / FUN_08A4CCE8 / FUN_08A4DC40 / FUN_08A4E1D8 also call FUN_08850574. That is probably where Gilgamesh's EX-Mode weapons come from: no retail timeline spawns ids 23–53.
- Gilgamesh data: `0x075A {37,15,1,…}` and `{61,15,1,…}` (pad 256 = outside EX Mode) hold two copies of wpnp row 15 with ids 37 and 61.
- The prop's per-tick code:
  - FUN_08850E00 (vtable entry at 0x089C8B04) applies hits and volumes.
  - FUN_088501E4 is the state machine (fades).
  - FUN_08850F44 → FUN_08833404 updates alpha.

---

### 0x025E — Gilgamesh weapon prop: hit volume on (weapon_volume for a held prop)
**Kind / args:** ranged, `{volumeSlot, propId}`.
- volumeSlot is an index into the prop definition's s8 table at def+0x30. Retail uses 1, 2 and 3.
- propId: 37/61 (hands), or 23/25/27/49/51/53 (EX Mode only).

**What the handler does (0x0882C8A8):** fighters only. It calls FUN_0884F3E8(actor+0xD64, slot, id).
- That finds the first active entry with id == propId (FUN_0884EDC0).
- It then calls FUN_0884ED94 → **FUN_08850FC0(prop, def+0x30[slot])**.
- FUN_08850FC0 appends the volume group to the prop's list at prop+0x5F8 (s16 × 8, count at +0x608).

**Who uses it / effect in game:** FUN_08850E00 (the prop object's per-tick method, vtable 0x089C8B04).
- For each listed group g ≥ 0, it ORs `1<<g` into **prop+0x448/+0x44C** (64-bit shift FUN_089985B0 with 1:0 from 0x089C8CE8).
- It then clears the count (0x08850E88–EF4).
- prop+0x448/+0x44C is the same "active hit volumes" mask that `weapon_volume` 0x0112 (0x088299E8) and `hit_window` (FUN_08833B80) set on fighters.
- So the weapon's own hit volume is on for exactly the ticks of the range.

**Data check:** 297 uses, all fiv210 (217) and fiv210_a (80). They pair with 0x045B and wrap it.
- fiv210 motion 39 (scripts 810/820): `{1,37}` and `{1,61}` at 20–32. The weapon hit 0x045B `{1979,61,7,0}` is at 22–28, and Gilgamesh's own 0x0200 `{1979,5}` is at 24–28.
- fiv210_a motion 8 (10005): `{1,37}`, `{1,61}`, plus 6 EX-only copies (`pad` 512) for ids 23/49/25/51/27/53 at 16–26.
- Slot 2/3 versions appear on other swings.

**Confidence:** confirmed for the mechanism (code end to end down to the volume mask). The meaning of the def+0x30 table (which mesh volume of the weapon) is strong; I can't read the wpnp rows statically.

**In-game test:** on a Gilgamesh swing, delete the 0x025E pair. The weapon should stop pushing/colliding during the swing, while the 0x045B damage hit still tries to fire. Or change slot 1 → 2 and the hit reach should change.

---

### 0x045B — Gilgamesh weapon prop: damage hit (hit_window for a held prop, with per-weapon damage profile)
**Kind / args:** ranged, `{atkpId, propId, volumeSlot, weaponMod}`.
- atkpId: the owner's atkp, e.g. 1979, 2101, 2189–2294, 2600, 2610.
- volumeSlot: an index into def+0x30, 0–7.
- weaponMod: an index into the def's s16 table at def+0x0C, i.e. an atkp in the **prop's own** table; 0–5 seen.

**What the handler does (0x0882C868):** fighters only. FUN_0884F324(list, atkp, slot, mod, id) finds the first active prop with that id, then calls FUN_0884ED1C:
1. **FUN_08850FAC(prop, atkp, def+0x30[slot])**: prop+0x5F4 = atkp, prop+0x5F6 = volume group, prop+0x60E = 1 (armed this tick).
2. **FUN_0884FE10(entry, atkp, mod)**. Once per continuous range (entry+0x1C; FUN_088501E4 clears it at 0x08850544–558 the first tick the range is absent), it builds a custom 0x44-byte hit profile:
   - It copies the owner's atkp (FUN_08837F14(owner, atkp)) and takes byte +0xC and the power (+0x10) from the prop's own atkp `def+0xC[mod]`. It adds +0xD, +0x13 and +7.
   - It then applies the def's flags at def+6:
     - bit1: random roll against def+0x38[0]% → power × def+0x38[1]/100, otherwise × def+0x38[2]/100, and +0xC = def+0x38[3];
     - bit0: power = −3;
     - bit2: random chance def+0x38[0]% → power = −1, unless the owner has +0x384 bit 0x800000;
     - bit3: power scaled by a factor from the owner's stats at +0x52C (+8 / +0xC, probably HP / max HP) using def+0x38[0..1];
     - power 0 → 1.
   - It stores the result with FUN_08850FF4 (copies it into the prop and sets prop+0x60F = 1).
   - The rand is the LCG at 0x09AF84D0.
   - The meaning of the negative power codes (−1/−3) is unknown; they look like special-damage markers.
3. entry+0x1D = 1.

**Who uses it / effect in game:** FUN_08850E00 at 0x08850E04–E84.
- If prop+0x60E is set, it calls **FUN_08833B80(prop, atkp or −1 = "use the stored custom profile" when +0x60F is set, group)**. That is the same routine the fighter's `hit_window` 0x0200 uses (0x08829AC4): it sets the volume bit and calls vtable +0xB8 (attack).
- Then FUN_0882FD08(prop+0x130, 0, 1), and it resets 0x5F4/0x5F6 = −1 and 0x60E = 0.
- So the **weapon prop itself deals the hit**, using a damage profile that depends on which weapon it is. Weapons with the random flag roll once per swing.

**Data check:** 220 uses, all fiv210 (163) / fiv210_a (57); 160 distinct `{atkp,id,slot}` combos.
- motion 39 `{1979,61,7,0}` 22–28 on the left weapon only.
- motion 43 (script 10000) `{2101,37,0,3}` / `{2101,61,0,3}` (pad 256 = outside EX Mode).
- fiv210_a 8 (10005): EX-only `{2232,23|25|…,0,4}` (pad 512).
- The ranges sit inside the matching 0x025E ranges and the swing's sound keys. This is why one character has hundreds: every swing repeats it per weapon id, and again for the EX-Mode set.

**Confidence:** confirmed for "arms a hit on the prop with id X using atkp + the prop's modifier". Strong for the flag/random semantics; the exact damage meaning of −1/−3 is unknown.

**In-game test:** in motion 39, change `{1979,61,7,0}` to a much stronger atkp. The left weapon's hit should do that damage. Then remove the event: only the body hit (0x0200 at 24–28) should remain. Swing many times with a random-flag weapon and watch the damage vary between swings but not within one.

---

### 0x055F — weapon trail / bone effect on a held prop (5-arg `bone_effect`)
**Kind / args:** ranged, `{slot 0–15, record (9999 = trail), recordBit, scale (float, 0 = 1.0), propId}`. propId = −1 means any prop.

**What the handler does (0x0882A918):** fighters only. scale = arg3 if the op has ≥ 4 args and arg3 ≠ 0.0, else 1.0. It calls **FUN_08B33CF4(actor, slot, record, bit, f12 = scale, t0 = propId)**:
- It loops over the 16 entries of actor+0xD64 (if any are active).
- For each with id == propId (or propId < 0) it calls **FUN_08B2E1A8(actor, slot, record, bit, scale, t0 = prop object)**.
- That sets bit `slot` in actor+0x6D8 and writes {record, bit, scale, prop object} to actor+0x5D8+slot*16. This is exactly what `bone_effect` 0x042A does, except that +0x5E4 holds the prop object instead of 0.

**Who uses it / effect in game:** FUN_0884E660 (per frame, from FUN_088413D0); see out1/D.md 0x042A.
- When the slot's +0xC (the object) is non-zero, it uses **that object's** effect set (`*(*(obj+0x390)+0x34)+0x28`) and its model attach (obj+0x4C8) at 0x0884E758–E784.
- So the trail record (9999, bit n) is looked up in the **weapon prop's** effect data and attached to the weapon prop's bone.
- It stops when the range ends or the script changes.
- This corrects round 1's "not seen in our data": it is Gilgamesh's.

**Data check:** 245 uses, fiv210 (194) / fiv210_a (51).
- motion 39: slots 0–7 `{s,9999,0,0,id}` for ids 37, 61, 23, 49, 25, 51, 27, 53 at 16–42, plus his own 0x042A `{0,9999,0,0}` at 16–42.
- fiv210_a 8: slots 0–15 over the 8 ids, with pad 512 (EX only) on the EX ids.
- Trails on props that don't exist that tick simply match nothing.

**Confidence:** confirmed (mechanism). "9999 = trail" is as strong as in round 1.

**In-game test:** in EX Mode, delete the 0x055F events from one Gilgamesh swing. The extra arms' weapons should swing without trails while his main trail (0x042A) stays.

---

### 0x0360 — fade a held prop out for the range, fade it back in after
**Kind / args:** ranged, `{propId, fadeInTicks, fadeOutTicks}`. 0 ticks = instant.

**What the handler does (0x0882C8D4):** fighters only. FUN_0884F7C4(list, id, a, b) → FUN_0884EEE0 on the first active matching entry: **entry+0x19 = 1, +0x14 = fadeIn, +0x16 = fadeOut**.

**Who uses it / effect in game:** the prop state machine FUN_088501E4, state 1 (0x08850238–2A4):
- If +0x19 is set: **FUN_08834D30(prop, fadeOut, 0)**, which sets the alpha rate prop+0x3B4 = −1/fadeOut and target +0x3B8 = 0. Then +0x19 = 0 and +0x1A = 1.
- Else if +0x1A: **FUN_08834D60(prop, fadeIn)**, which sets the rate +1/fadeIn.
- The ranged event re-arms +0x19 every tick, so the prop fades out (and stays out) for the whole range and fades back in once the range ends.
- The alpha integrator is FUN_08833404 (0x0883346C–351C). It clamps prop+0x3B0 to [target, 1] and copies it to +0x370, the draw alpha. FUN_08850F44 does the same for props, and also takes the owner's alpha if lower.

**Data check:** 37 uses: fiv210 36, ten110 (Yuna) 1.
- Gilgamesh motion 49 (script 10015, a projectile move: keyed 0x0204 `{1796,0}` at 30): `{37,8,8}` and `{61,8,8}` at 6–90 (EX only). Keyed effects 65/66 at 6 and 90 (puffs) mark the swords vanishing and returning.
- ten110 motion 55 (10040): `{55,10,0}` 0–22 hides the summon/prop that 0x0C65 `{55,48,…}` spawns at tick 0, then fades it in over 10 ticks.

**Confidence:** confirmed.

**In-game test:** add `{37,30,30}` over 0–60 to a Gilgamesh stand motion. The right-hand weapon should fade out over 0.5 s and back in over 0.5 s after tick 60.

---

### 0x0D64 / 0x0D63 — bone aim (upper body / arms track the target)
**Kind / args:** ranged, 13 args. The slot offsets are in the manager record (actor+0x1120, 3 slots × 0x60 from +0x10).
| arg | meaning |
|---|---|
| 0 | bone A index (model bone, FUN_089B0E78); −1 = none |
| 1 | float weight A |
| 2 | bone B index, −1 = none |
| 3 | float weight B |
| 4 / 5 | float yaw limits left / right (degrees) |
| 6 / 7 | float pitch limits up / down (degrees; 0 = no vertical aim) |
| 8 | float turn speed, degrees per tick (180 = snap) |
| 9 | float hold time (ticks) after the range ends before relaxing |
| 10 | float follow smoothing (0–1, higher = follows faster; per tick `1 − (1−a)^(dt/2)`) |
| 11 | float relax factor (per tick `cur *= a^(dt/2)` when going back to rest; lower = faster) |
| 12 | 1 = **player-steered** aim instead of auto-tracking |

**What the handler does:** 0x63 at 0x0882C980, 0x64 at 0x0882C9FC. Fighters only.
- It sets **actor+0x197 bit 0x04**. I found no reader of this bit in the scan.
- It calls FUN_08A627EC (0x0D63, type 1) / FUN_08A62978 (0x0D64, type 2) on **actor+0x1120**. Those pick or reuse a slot keyed by (type, boneA, boneB) via FUN_08A62FA0, store the args, and set +0x54 = 1, +0x58 = 0 (tracking), +0x5C = 0.
- The ranged event refreshes this every tick.

**Who uses it / effect in game:**
- **FUN_08A6307C** (per tick from the fighter update FUN_08B37604 at 0x08B379F8), per slot:
  - **state 0 (in range):**
    - arg12 = 1: yaw/pitch move with the actor's +0x5B0 bits 0x40/0x80 (yaw) and 0x100/0x200 (pitch), presumably directional input (weak), at arg8 °/tick within the limits.
    - Otherwise it aims at **type 2 (0x0D64): actor+0x2FC (current target, vtable+0x120)**; with no target it relaxes. **type 1 (0x0D63): actor+0x300 (1v1 opponent, vtable+0x130)**.
    - It computes the yaw/pitch to the target position in the actor's local frame, clamps the change to arg8 °/tick and the total to ±limits, then sets state 1.
  - **state 1:** hold for arg9 ticks.
  - **state 2:** multiply by arg11 each tick until near zero, then free the slot.
  - It keeps a smoothed copy (+0x10/+0x14) with arg10.
  - If the fighter is neither in an action nor `can_act` (+0x190 bit 1), it frees the slot (0x08A63710–3750).
- **FUN_08A62390** (from FUN_08B30814 at 0x08B3085C, the pose/draw step): it rotates bone A and bone B by smoothed yaw/pitch × their weights (pitch is weighted by w/Σw).
- **FUN_08A62F00** (from FUN_08B34C94) returns yaw × (1 − Σweights), i.e. the share of the turn not taken by the bones.

**Data check:** 0x0D64 is used 36 times, 0x0D63 twice (both Laguna).
- **eht110 = Laguna:** all his gun motions. 43–49 (10000–10006 / 20040–20046) use `{20,0.6,21,0.4, 20,20, 0,0, 1–4, 30, 0.7,0.7, 0}` over the whole motion. 54 (10040) starts with a 180°/tick snap at 0–2, then 2°/tick. 10010 and 10050 use arg12 = 1 (steered, with pitch limits 30/45).
- 0x0D63 in 58/59 (10060–10063) aims at the 1v1 opponent instead.
- one100 (WoL) 47/62, two100 (Firion) 46/50 (bow), for100 (Cecil) 48: short ranges at the start of ranged/aimed moves.
- two200 (Emperor) 47/48: steered, pitch ±45.

**Confidence:** confirmed for the mechanism (args → bone rotation toward target). Weak for "arg12 = player input steering", because I didn't find the writer of the +0x5B0 direction bits. Unknown for +0x197 bit 0x04.

**In-game test:** give a Noctis attack 0x0D64 `{spineBone,1.0,-1,0, 60,60, 0,0, 180, 30, 0.9,0.8, 0}` with the foe off to the side. The torso should twist toward the foe, stay there 30 ticks after the range, then ease back. Then set arg4/5 = 0 and the twist should be gone.

---

### 0x047A — can act now, only if the move hit (arg2 = 0) or only if it whiffed (arg2 = 1)
**Kind / args:** ranged, `{a0, a1, a2 = condition, a3}`. Retail values: `[1,0,1,0]`, `[1,1,0,0]`, `[1,0,0,0]`, `[0,0,0,0]`, `[1,0,1,1]`, `[0,0,1,0]`.

**What the handler does (0x0882B090):**
- Gate on **actor+0x380 bit 0x100**, which the attacker's on-hit handler FUN_08838734 sets (with 0x200/0x800) when its hit lands:
  - **arg2 = 0 → only when the hit landed**;
  - **arg2 ≠ 0 → only when it didn't**.
  - Otherwise nothing happens.
- Then it sets the same bits as `can_act_now` 0x041E (0x0882AF34), with the args shifted:
  - always **+0x193 |= 1 and +0x190 |= 1** (can act);
  - a0 ≠ 0 → +0x193 |= 2, +0x199 |= 4;
  - a1 ≠ 0 → also +0x198 |= 0x40;
  - a3 ≠ 0 → also +0x198 |= 0x80.

**Who uses it / effect in game:** the readers are listed in out1/D.md (0x041E).
- **FUN_08B2F5E0** returns "may act" at once when +0x193 bit 0 is set and +0x199 bit 4 is not. So **a0 = 0 = instant cancel** (skips the idle-state checks), while a0 = 1 leaves only the normal `can_act` path.
- +0x190 bit 1 opens attack, block, dodge and dash (out1/A.md).
- +0x198 bit 0x40 is read in the action starter FUN_08B382B8 at 0x08B3837C. When set, the new action is chained from the current one: the current record is saved to +0x1024/+0x101C. The exact meaning is weak.
- +0x198 bit 0x80 is read only by the AI (0x08A7D298).

**Data check:** 24 uses across 16 fighters, nearly always open-ended near the end of an attack.
- two100 motion 44 (10001/10003): `[1,0,1,0]` and `[1,1,0,0]` both from 38, pad 2. Whiff and hit get different cancel rules.
- for100 47 (10002): `[0,0,0,0]` from 46 = instant cancel on hit.
- ten200 (Jecht) 44/45/51: `[1,0,1,1]` bounded 36–48 / 56–66 (whiff only).
- gst130 55 (15 scripts): `[0,0,1,0]` from 28 = instant cancel on whiff.

**Confidence:** strong. The hit/whiff gate and the bits are confirmed; what each extra bit does in play is only partly known, as in 0x041E.

**In-game test:** give a Noctis attack `0x047A [0,0,0,0]` from a tick just after its hit window. On hit you should be able to attack or dodge right away from that tick; on a whiff, not. Then switch arg2 to 1 and the behaviour should flip.

---

### 0x003E — EX Burst: release the position lock (fighters move freely again)
**Kind / args:** ranged, no args. Sets **actor+0x194 bit 0x02** (0x0882B2A8).

**Who uses it / effect in game:** the only reader is **FUN_08A3D904** (0x08A3D918). FUN_08A3FE10 (the EX Burst manager, scripts 8000–8700 = 0x1F40–0x21FC; called from FUN_088413D0) calls it each tick with attacker = mgr+0x26C and victim = mgr+0x270.
- **Normally** (bit off):
  - The victim gets +0x384 |= 0x1000. FUN_0883752C reads that bit as "skip floor clamp / counts as airborne", the same check as air_motion.
  - If the victim isn't at the origin, it is placed at the burst point (global vector 0x09AF84C0, FUN_088359A8 = set position).
  - The attacker is placed at an offset from the victim (FUN_08835134 locator + the mgr+0x298 × attacker+0x2CC scale).
- **0x003E set:** it only clears the victim's +0x384 bit 0x1000 and returns, so neither fighter is re-placed. The victim falls and lands normally, and the attacker moves under its own motion.
- The sibling bit +0x194 bit 0x01 (op 0x0031) also releases, but first moves the victim by +0.9 on one axis and calls its vtable +0xE8.

**Data check:** 17 uses, all in EX Burst scripts, from a late tick to the end:
- one200 (Garland) 8104/8105 from 14, 8500 from 108;
- sev100 (Cloud) 8103–8112 from 24;
- two200 8501–8503 from 40/98;
- thr200 8500 from 214;
- eht200 8500 from 36.

That is the finishing phase, where the victim is launched or dropped.

**Confidence:** strong. The reader and the release are confirmed; "position lock" is my reading of the else-branch (the set-position calls are certain, the exact anchor points are not).

**In-game test:** remove 0x003E from Cloud's Omnislash finisher (sev100 motion 73, 8103). The foe should stay pinned in mid-air at the burst point instead of dropping or flying off. Adding it at tick 0 should let both drift out of position during the burst.

---

### 0x0340 — fade the fighter (transparency) for the range
**Kind / args:** ranged, `{fadeOutTicks, fadeBackTicks (0 → 2), opacity % during the range}`.

**What the handler does (0x0882BC18):**
- Unless +0x195 bit 0x08 is already set this tick, it stores **actor+0x2A8 = float(fadeBack or 2)** and sets **+0x195 |= 0x08**.
- It then calls **FUN_08834D30(actor, fadeOut, opacity/100)**: alpha rate +0x3B4 = −1/fadeOut (−1 = instant if 0) and floor +0x3B8 = opacity/100.

**Who uses it / effect in game:** **FUN_08833404** (actor alpha update; base vtable entry at 0x089C775C via FUN_088281E0).
- While +0x195 bit 0x08 is set it leaves the fade running down to the floor.
- On the first tick without the bit and with +0x2A8 > 0, it calls FUN_08834D60(actor, +0x2A8) (rate +1/ticks) and clears +0x2A8.
- The alpha +0x3B0 is integrated, clamped to [floor, 1], and written to +0x370 (draw alpha); 0x0883C6AC also tests alpha ≤ 0.
- So: **fade to X% over N ticks, hold for the range, fade back to 100% over M ticks after it.**

**Data check:** 15 uses:
- for200 (Golbez) scripts 100/103 `{6,6,0}` 0–12 (his teleport-style movement) and 10004 `{4,4,0}`;
- sev110 10000–10013 `{4,4,0}` 0–14, together with 0x002F over 0–24 (vanish-dash attacks);
- two200 8103 `{0,0,60}` whole motion (60% opacity), 8501–8503 `{0,40,0}` 0–80 (invisible, then 40-tick fade-in);
- gst130 10074 `{8,8,25}`.

**Confidence:** confirmed.

**In-game test:** add `{10,20,0}` over 0–30 to a Noctis attack. He should fade out over 10 ticks, stay invisible to tick 30, then fade in over 20 ticks. `{0,0,50}` should give instant 50% see-through.

---

### 0x002F — counts as evading (treated like a dodge/guard)
**Kind / args:** ranged, no args. Sets **actor+0x194 bit 0x10** (0x0882B6FC).

**Who uses it / effect in game:** the only reader is **FUN_08837458** "is evading" = script 400–469 (FUN_08837430) **or** +0x194 bit 0x10. Its callers:
- the dispatcher's turn handlers (0x08829F78, 0x0882A10C, 0x0882A260: `turn_to_opponent` arg2/arg3 "don't turn while the foe evades", out1/D.md);
- FUN_0883AF80 at 0x0883B23C: an evading fighter's external push vector (actor+0x120 × +0x12C) is **halved** (0.5 at 0x089C7EDC);
- FUN_0883F69C at 0x0884068C, the hit/collision loop: when the other side is evading it scales a value by a % (FUN_0883ECD8) and marks bit s2 in +0x5E8;
- the dodge gate FUN_08B3A5BC (0x08B3A75C);
- FUN_08B2DC38;
- the player handler FUN_08B39024 (0x08B39154);
- many CPU-AI predicates (0x08A60F90, 0x08A66164, 0x08A70708 …): the AI reacts as if you dodged.

It does **not** make the fighter invincible; that is 0x0006.

**Data check:** 12 uses, always from tick 0 at the start of special moves:
- for110 (Kain) 10051 0–20;
- ten100 (Tidus) 10097–10100 0–40;
- thr200 10097 0–30;
- sev110 10000–10013 0–24 (with the 0x0340 fade);
- fiv210 10045 0–10;
- gst110 10067, gst120 10100 0–20.

These are moves that open with a vanish or evasive movement.

**Confidence:** confirmed (flag → "is evading" predicate). The end effect is spread across AI and turning; the hit-loop use (0x0884068C) is weak.

**In-game test:** give a Noctis attack 0x002F over 0–60 and have a CPU foe use a move with `turn_to_opponent` arg2 = 1. It should stop tracking Noctis during those ticks, as it does while he dodges. A CPU should also react to him as if he were evading.

---

### 0x0524 — mash ○ (or □) to fire extra projectiles during the range
**Kind / args:** ranged, `{magiId, magiArg, cooldownTicks, slot 0–3, button (0 = ○ Brave, 1 = □ HP)}`.

**What the handler does (0x0882B208):** it sets **+0x193 |= 0x20** and calls **FUN_08836D38(actor, slot, magiId, magiArg, cooldown, button≠0)**. If the slot `*(actor+0x4AC) + slot*0x14` is idle, that fills in {magi, arg, float cooldown, timer 0, active = 1, button}. The 4 slots are allocated in FUN_08B3A3xx (0x08B3A430).

**Who uses it / effect in game:** the player input handler **FUN_08B3526C at 0x08B35408–5514** (CPU AI reads +0x193 bit 0x20 at 0x08A9DE28 / 0x08A9E1CC / 0x08A9E650 / 0x08A9E8DC). For each active slot:
- While +0x193 bit 0x20 is on and the timer is 0, it checks the button: FUN_08B30E10 (input id 0xA, ○) or FUN_08B309C8 (id 0xB, □).
- On a press it calls **vtable+0x118(actor, magiId, magiArg)**, the same call keyed `fire_projectile` 0x0204 makes (0x0882D314), and sets the timer to the cooldown.
- The timer counts down by the time step. When the range ends the slot is freed (0x08B35504).

**Data check:** 12 uses, all casters, over long middle stretches of projectile moves:
- six100 (Terra) 10030 `{1040,0,30,0,0}` 170–200, 10043 `{235,0,30|10,…}`;
- six200 (Kefka) 10021/10024 `{366|976,0,10,…}` 42–142;
- eht200 (Ultimecia) 10000 `{101,0,8,…}` 48–152, 10040/10041 two slots `{225,…,0}` / `{226,…,1}`; eht200_a `{1970,0,2,0,1}` uses □;
- for200 (Golbez) 10020 `{534,0,30,…}`.

These are the "press repeatedly for more shots" moves.

**Confidence:** confirmed (input → projectile spawn).

**In-game test:** on a Noctis or Vivi move, add `{<a magi id the moveset has>, 0, 15, 0, 0}` over a 120-tick stretch and tap ○. You should get one extra projectile per press, at most one every 15 ticks. With button = 1 it should respond to □ instead.

---

## Notes for other agents / corrections
- out1/D.md said 0x055F is "not seen in our data". It is Gilgamesh's (245 uses), and t0 is the prop object found by id.
- New engine facts:
  - actor+0xD64 = held-prop list (16 × 0x20); actor+0x1120 = bone-aim manager (3 slots × 0x60).
  - actor+0x4AC = mash-fire slots (4 × 0x14).
  - Alpha: actor+0x3B0 (current), +0x3B4 (rate/tick), +0x3B8 (floor), +0x370 (draw); setters FUN_08834D30 (fade out), FUN_08834D60 (fade in), FUN_08834DF4 (set).
  - EX Burst manager FUN_08A3FE10 (attacker +0x26C, victim +0x270, state +0x278).
  - +0x380 bit 0x100 = "this action's hit landed".
  - Event `pad` high byte 1/2 = outside/inside EX Mode.
- Op 0x0031 (+0x194 bit 0x01) = EX Burst release variant (3 uses: eht100 8102, ten200 8101), traced in passing.
